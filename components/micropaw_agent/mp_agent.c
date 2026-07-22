#include "mp_agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "mp_config.h"
#include "mp_confirmation.h"
#include "mp_json.h"
#include "mp_llm.h"
#include "mp_memory.h"
#include "mp_metrics.h"
#include "mp_scheduler.h"
#include "mp_tools.h"
#include "sdkconfig.h"

extern const char system_txt[] asm("_binary_system_txt_start");
extern const char scheduled_txt[] asm("_binary_scheduled_txt_start");

#define AGENT_QUEUE_LENGTH 4
#define AGENT_REQUEST_LEN (CONFIG_MICROPAW_WORK_TEXT_BYTES * 6 + 65536)
#define AGENT_TRACE_LEN (CONFIG_MICROPAW_WORK_TEXT_BYTES * 4 + 32768)
#define AGENT_INSTRUCTIONS_LEN (MP_MEMORY_SLOTS * (MP_MEMORY_TEXT_LEN + 3) + 4096)

EXT_RAM_BSS_ATTR static char s_request[AGENT_REQUEST_LEN];
EXT_RAM_BSS_ATTR static char s_instructions[AGENT_INSTRUCTIONS_LEN];
EXT_RAM_BSS_ATTR static char s_memory_context[MP_MEMORY_SLOTS * (MP_MEMORY_TEXT_LEN + 3)];
EXT_RAM_BSS_ATTR static char s_tools[32768];
EXT_RAM_BSS_ATTR static char s_tool_trace[AGENT_TRACE_LEN];
EXT_RAM_BSS_ATTR static char s_tool_output[MP_TOOL_RESULT_LEN];
EXT_RAM_BSS_ATTR static char s_confirm_tool[MP_TOOL_NAME_LEN];
EXT_RAM_BSS_ATTR static char s_confirm_arguments[MP_TOOL_ARGS_LEN];
EXT_RAM_BSS_ATTR static mp_llm_result_t s_result;
static StaticQueue_t s_queue_buffer;
static uint8_t s_queue_storage[AGENT_QUEUE_LENGTH * sizeof(mp_message_t)];
static QueueHandle_t s_queue;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[CONFIG_MICROPAW_AGENT_STACK];
static TaskHandle_t s_task;
static mp_send_fn s_send;
static volatile mp_agent_state_t s_state = MP_AGENT_IDLE;
static const char *TAG = "agent";

static void agent_task(void *argument);
static void process_message(const mp_message_t *message);
static bool handle_command(const mp_message_t *message);
static bool build_instructions(bool proactive);
static bool build_request(const mp_message_t *message);
static bool append_tool_exchange(const mp_llm_result_t *result, const char *output);
static bool append_history(mp_writer_t *writer, const char *chat_id);
static void send_reply(const char *chat_id, const char *text);
static void execute_confirmation(const mp_message_t *message, uint32_t id);
esp_err_t mp_agent_init(mp_send_fn send);
esp_err_t mp_agent_start(void);
esp_err_t mp_agent_submit(const char *chat_id, const char *text, bool proactive, TickType_t timeout);
mp_agent_state_t mp_agent_state(void);
TaskHandle_t mp_agent_task_handle(void);

static void agent_task(void *argument)
{
    (void)argument;
    mp_message_t message;
    while (true) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) == pdTRUE) {
            process_message(&message);
            s_state = MP_AGENT_IDLE;
        }
    }
}

static void process_message(const mp_message_t *message)
{
    if (handle_command(message)) {
        return;
    }
    if (message->proactive) {
        mp_tool_context_t context = {0};
        strlcpy(context.chat_id, message->chat_id, sizeof(context.chat_id));
        s_tool_output[0] = 0;
        esp_err_t error = mp_tools_execute_scheduled(message->text, &context,
                                                     s_tool_output, sizeof(s_tool_output));
        if (error != ESP_ERR_NOT_FOUND) {
            if (error != ESP_OK && !s_tool_output[0]) {
                snprintf(s_tool_output, sizeof(s_tool_output), "Tool error: %s", esp_err_to_name(error));
            }
            send_reply(message->chat_id, s_tool_output);
            return;
        }
    }
    if (!mp_llm_ready()) {
        send_reply(message->chat_id, "The LLM provider is not configured. Check llm_provider, llm_model, llm_api_key and llm_endpoint.");
        return;
    }
    s_state = MP_AGENT_CONTEXT;
    s_tool_trace[0] = 0;
    if (!mp_tools_json(s_tools, sizeof(s_tools)) || !build_instructions(message->proactive) ||
        !build_request(message)) {
        s_state = MP_AGENT_ERROR;
        send_reply(message->chat_id, "The request exceeded the device buffer.");
        return;
    }
    while (true) {
        s_state = MP_AGENT_INFERENCE;
        esp_err_t error = mp_llm_stream(s_request, &s_result);
        if (error != ESP_OK) {
            s_state = MP_AGENT_ERROR;
            send_reply(message->chat_id, s_result.error[0] ? s_result.error : esp_err_to_name(error));
            return;
        }
        if (!s_result.has_tool) {
            s_state = MP_AGENT_RESPONSE;
            send_reply(message->chat_id, s_result.text);
            if (!message->proactive) {
                mp_history_add_exchange(message->chat_id, message->text, s_result.text,
                                        s_result.message_id);
            }
            return;
        }
        s_state = MP_AGENT_TOOL;
        mp_tool_context_t context = {0};
        strlcpy(context.chat_id, message->chat_id, sizeof(context.chat_id));
        s_tool_output[0] = 0;
        error = mp_tools_execute(s_result.tool, s_result.arguments, &context, false,
                                 s_tool_output, sizeof(s_tool_output));
        if (error != ESP_OK && !s_tool_output[0]) {
            snprintf(s_tool_output, sizeof(s_tool_output), "Tool error: %s", esp_err_to_name(error));
        }
        if (!append_tool_exchange(&s_result, s_tool_output) || !build_request(message)) {
            s_state = MP_AGENT_ERROR;
            send_reply(message->chat_id, "The tool result exceeded the device buffer.");
            return;
        }
    }
}

static bool handle_command(const mp_message_t *message)
{
    if (strcmp(message->text, "/help") == 0 || strcmp(message->text, "/start") == 0) {
        send_reply(message->chat_id, "MicroPaw is ready. Commands: /metrics, /memory, /jobs, /forget, /reset-state YES, /confirm ID, /cancel ID.");
        return true;
    }
    if (strcmp(message->text, "/metrics") == 0) {
        mp_metrics_format(s_tool_output, sizeof(s_tool_output));
        send_reply(message->chat_id, s_tool_output);
        return true;
    }
    if (strcmp(message->text, "/memory") == 0) {
        mp_memory_format(s_tool_output, sizeof(s_tool_output));
        send_reply(message->chat_id, s_tool_output[0] ? s_tool_output : "No saved memory.");
        return true;
    }
    if (strcmp(message->text, "/jobs") == 0) {
        mp_scheduler_format(s_tool_output, sizeof(s_tool_output));
        send_reply(message->chat_id, s_tool_output);
        return true;
    }
    if (strcmp(message->text, "/forget") == 0) {
        esp_err_t error = mp_history_clear(message->chat_id);
        send_reply(message->chat_id, error == ESP_OK ? "Recent conversation cleared." : esp_err_to_name(error));
        return true;
    }
    if (strcmp(message->text, "/reset-state YES") == 0) {
        esp_err_t error = mp_memory_reset();
        if (error == ESP_OK) {
            error = mp_scheduler_reset();
        }
        if (error == ESP_OK) {
            mp_confirmation_reset();
        }
        send_reply(message->chat_id, error == ESP_OK ? "Memory and jobs cleared." : esp_err_to_name(error));
        return true;
    }
    unsigned long parsed;
    if (sscanf(message->text, "/confirm %lu", &parsed) == 1 && parsed <= UINT32_MAX) {
        execute_confirmation(message, (uint32_t)parsed);
        return true;
    }
    if (sscanf(message->text, "/cancel %lu", &parsed) == 1 && parsed <= UINT32_MAX) {
        esp_err_t error = mp_confirmation_cancel(message->chat_id, (uint32_t)parsed);
        send_reply(message->chat_id, error == ESP_OK ? "Permission cancelled." : "Permission not found or expired.");
        return true;
    }
    return false;
}

static bool build_instructions(bool proactive)
{
    mp_memory_format(s_memory_context, sizeof(s_memory_context));
    mp_writer_t writer;
    mp_writer_init(&writer, s_instructions, sizeof(s_instructions));
    mp_writer_raw(&writer, system_txt);
    if (proactive) {
        mp_writer_char(&writer, '\n');
        mp_writer_raw(&writer, scheduled_txt);
    }
    if (s_memory_context[0]) {
        mp_writer_raw(&writer, "\nSaved memory:\n");
        mp_writer_raw(&writer, s_memory_context);
    }
    return writer.valid;
}

static bool build_request(const mp_message_t *message)
{
    const mp_config_t *config = mp_config_get();
    mp_writer_t writer;
    mp_writer_init(&writer, s_request, sizeof(s_request));
    mp_writer_raw(&writer, "{\"model\":");
    mp_writer_string(&writer, config->llm_model);
    if (strcmp(config->llm_model, "gpt-5.6-luna") == 0 ||
        strcmp(config->llm_model, "openai/gpt-5.6-luna") == 0) {
        mp_writer_raw(&writer, ",\"reasoning\":{\"effort\":\"none\"}");
    }
    mp_writer_raw(&writer, ",\"instructions\":");
    mp_writer_string(&writer, s_instructions);
    mp_writer_raw(&writer, ",\"input\":[");
    bool has_history = !message->proactive && append_history(&writer, message->chat_id);
    if (has_history) {
        mp_writer_char(&writer, ',');
    }
    mp_writer_raw(&writer, "{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":");
    mp_writer_string(&writer, message->text);
    mp_writer_raw(&writer, "}]}");
    if (s_tool_trace[0]) {
        mp_writer_char(&writer, ',');
        mp_writer_raw(&writer, s_tool_trace);
    }
    mp_writer_raw(&writer, "],\"tools\":");
    mp_writer_raw(&writer, s_tools);
    mp_writer_format(&writer, ",\"parallel_tool_calls\":false,\"stream\":true,\"store\":false,\"max_output_tokens\":%lu}",
                     strtoul(config->llm_max_output_tokens, NULL, 10));
    return writer.valid;
}

static bool append_tool_exchange(const mp_llm_result_t *result, const char *output)
{
    size_t used = strnlen(s_tool_trace, sizeof(s_tool_trace));
    if (used >= sizeof(s_tool_trace)) {
        return false;
    }
    mp_writer_t writer;
    mp_writer_init(&writer, s_tool_trace + used, sizeof(s_tool_trace) - used);
    if (used) {
        mp_writer_char(&writer, ',');
    }
    mp_writer_raw(&writer, "{\"type\":\"function_call\",\"id\":");
    mp_writer_string(&writer, result->item_id);
    mp_writer_raw(&writer, ",\"call_id\":");
    mp_writer_string(&writer, result->call_id);
    mp_writer_raw(&writer, ",\"name\":");
    mp_writer_string(&writer, result->tool);
    mp_writer_raw(&writer, ",\"arguments\":");
    mp_writer_string(&writer, result->arguments);
    mp_writer_raw(&writer, "},{\"type\":\"function_call_output\",\"call_id\":");
    mp_writer_string(&writer, result->call_id);
    mp_writer_raw(&writer, ",\"output\":");
    mp_writer_string(&writer, output);
    mp_writer_char(&writer, '}');
    return writer.valid;
}

static bool append_history(mp_writer_t *writer, const char *chat_id)
{
    char roles[MP_HISTORY_SLOTS][10];
    char texts[MP_HISTORY_SLOTS][MP_HISTORY_TEXT_LEN];
    char ids[MP_HISTORY_SLOTS][MP_HISTORY_ID_LEN];
    size_t count = mp_history_get(chat_id, roles, texts, ids, MP_HISTORY_SLOTS);
    if (!count) {
        return false;
    }
    for (size_t index = 0; index < count; index++) {
        if (index) {
            mp_writer_char(writer, ',');
        }
        mp_writer_raw(writer, "{\"type\":\"message\",\"role\":");
        mp_writer_string(writer, roles[index]);
        if (strcmp(roles[index], "assistant") == 0) {
            mp_writer_raw(writer, ",\"id\":");
            mp_writer_string(writer, ids[index]);
            mp_writer_raw(writer, ",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":");
            mp_writer_string(writer, texts[index]);
            mp_writer_raw(writer, ",\"annotations\":[]}]}");
        } else {
            mp_writer_raw(writer, ",\"content\":[{\"type\":\"input_text\",\"text\":");
            mp_writer_string(writer, texts[index]);
            mp_writer_raw(writer, "}]}");
        }
    }
    return writer->valid;
}

static void send_reply(const char *chat_id, const char *text)
{
    if (s_send) {
        s_send(chat_id, text);
    } else {
        ESP_LOGI(TAG, "%s", text);
    }
}

static void execute_confirmation(const mp_message_t *message, uint32_t id)
{
    esp_err_t error = mp_confirmation_take(message->chat_id, id, s_confirm_tool,
                                            sizeof(s_confirm_tool), s_confirm_arguments,
                                            sizeof(s_confirm_arguments));
    if (error != ESP_OK) {
        send_reply(message->chat_id, "Permission not found or expired.");
        return;
    }
    mp_tool_context_t context = {0};
    strlcpy(context.chat_id, message->chat_id, sizeof(context.chat_id));
    s_tool_output[0] = 0;
    error = mp_tools_execute(s_confirm_tool, s_confirm_arguments, &context, true,
                             s_tool_output, sizeof(s_tool_output));
    send_reply(message->chat_id, error == ESP_OK || s_tool_output[0] ? s_tool_output : esp_err_to_name(error));
}

esp_err_t mp_agent_init(mp_send_fn send)
{
    s_send = send;
    s_queue = xQueueCreateStatic(AGENT_QUEUE_LENGTH, sizeof(mp_message_t), s_queue_storage, &s_queue_buffer);
    return s_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mp_agent_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_task = xTaskCreateStaticPinnedToCore(agent_task, "mp_agent", CONFIG_MICROPAW_AGENT_STACK,
                                           NULL, 5, s_task_stack, &s_task_buffer, 1);
    if (s_task) {
        mp_metrics_register("agent", s_task);
    }
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mp_agent_submit(const char *chat_id, const char *text, bool proactive, TickType_t timeout)
{
    if (!s_queue || !chat_id || !text || strlen(chat_id) >= MP_CHAT_ID_LEN ||
        strlen(text) >= MP_MESSAGE_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_message_t message = {.proactive = proactive};
    strlcpy(message.chat_id, chat_id, sizeof(message.chat_id));
    strlcpy(message.text, text, sizeof(message.text));
    return xQueueSend(s_queue, &message, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

mp_agent_state_t mp_agent_state(void)
{
    return s_state;
}

TaskHandle_t mp_agent_task_handle(void)
{
    return s_task;
}
