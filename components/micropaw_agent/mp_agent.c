#include "mp_agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mp_config.h"
#include "mp_confirmation.h"
#include "mp_context.h"
#include "mp_json.h"
#include "mp_llm.h"
#include "mp_memory.h"
#include "mp_metrics.h"
#include "mp_ota.h"
#include "mp_scheduler.h"
#include "mp_tools.h"
#include "sdkconfig.h"

extern const char system_txt[] asm("_binary_system_txt_start");
extern const char scheduled_txt[] asm("_binary_scheduled_txt_start");

#define AGENT_QUEUE_LENGTH 4
#define AGENT_INSTRUCTIONS_LEN (MP_MEMORY_SLOTS * (MP_MEMORY_TEXT_LEN + 3) + 4096)

typedef struct {
    uint8_t step;
    int64_t expires_us;
    char chat_id[MP_CHAT_ID_LEN];
} reset_confirmation_t;

EXT_RAM_BSS_ATTR static char s_request[MP_AGENT_REQUEST_LEN];
EXT_RAM_BSS_ATTR static char s_instructions[AGENT_INSTRUCTIONS_LEN];
EXT_RAM_BSS_ATTR static char s_memory_context[MP_MEMORY_SLOTS * (MP_MEMORY_TEXT_LEN + 3)];
EXT_RAM_BSS_ATTR static char s_tool_trace[MP_TOOL_TRACE_LEN];
EXT_RAM_BSS_ATTR static char s_tool_output[MP_TOOL_RESULT_LEN];
EXT_RAM_BSS_ATTR static char s_confirm_tool[MP_TOOL_NAME_LEN];
EXT_RAM_BSS_ATTR static char s_confirm_arguments[MP_TOOL_ARGS_LEN + 1];
EXT_RAM_BSS_ATTR static mp_llm_result_t s_result;
static StaticQueue_t s_queue_buffer;
static uint8_t s_queue_storage[AGENT_QUEUE_LENGTH * sizeof(mp_message_t)];
static QueueHandle_t s_queue;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[CONFIG_MICROPAW_AGENT_STACK];
static TaskHandle_t s_task;
static mp_send_fn s_send;
static mp_flush_fn s_flush;
static mp_finish_fn s_finish;
static size_t s_trace_length;
static reset_confirmation_t s_reset;
static volatile mp_agent_state_t s_state = MP_AGENT_IDLE;
static const char *TAG = "agent";

static void agent_task(void *argument);
static bool process_message(const mp_message_t *message);
static bool handle_command(const mp_message_t *message);
static bool owner_message(const mp_message_t *message);
static void reset_confirmation(void);
static esp_err_t reset_state(void);
static bool build_instructions(bool proactive);
static bool append_input_message(mp_writer_t *writer, const char *role,
                                 const char *text, const char *id);
static bool build_request(const mp_message_t *message);
static bool append_progress(const mp_llm_result_t *result);
static bool append_tool_exchange(const mp_llm_call_t *call, const char *output);
static bool persist_turn(const mp_message_t *message, const mp_llm_result_t *final_result,
                         size_t *turn_size);
static bool build_compaction_request(uint32_t remove_count);
static void compact_context(bool force);
static esp_err_t send_text(const char *chat_id, const char *text, bool final);
static void send_progress(const char *chat_id, const char *text);
static void execute_confirmation(const mp_message_t *message, uint32_t id);
static void ota_progress(const char *text, void *context);
static esp_err_t submit_message(uint32_t schedule_id, const char *chat_id, const char *text,
                                bool proactive, TickType_t timeout);
esp_err_t mp_agent_init(mp_send_fn send, mp_flush_fn flush, mp_finish_fn finish);
esp_err_t mp_agent_start(void);
esp_err_t mp_agent_submit(const char *chat_id, const char *text, bool proactive, TickType_t timeout);
esp_err_t mp_agent_submit_scheduled(uint32_t id, const char *chat_id, const char *text);
void mp_agent_reset_confirmation(void);
mp_agent_state_t mp_agent_state(void);
TaskHandle_t mp_agent_task_handle(void);

static void agent_task(void *argument)
{
    (void)argument;
    mp_message_t message;
    while (true) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) == pdTRUE) {
            mp_metrics_request_begin(message.queued_us);
            bool success = process_message(&message);
            if (message.schedule_id) {
                esp_err_t error = mp_scheduler_complete(message.schedule_id, success);
                if (error != ESP_OK) {
                    mp_metrics_error("scheduler", error);
                }
            }
            s_state = MP_AGENT_IDLE;
        }
    }
}

static bool process_message(const mp_message_t *message)
{
    if (handle_command(message)) {
        return true;
    }
    if (message->proactive) {
        mp_tool_context_t context = {0};
        strlcpy(context.chat_id, message->chat_id, sizeof(context.chat_id));
        s_tool_output[0] = 0;
        esp_err_t error = mp_tools_execute_scheduled(message->text, &context,
                                                     s_tool_output, sizeof(s_tool_output));
        if (error != ESP_ERR_NOT_FOUND) {
            if (error != ESP_OK) {
                mp_metrics_error("scheduled_tool", error);
                ESP_LOGW(TAG, "scheduled job %lu: %s",
                         (unsigned long)message->schedule_id, esp_err_to_name(error));
                return false;
            }
            send_text(message->chat_id, s_tool_output, true);
            return true;
        }
    }
    if (!mp_llm_ready()) {
        send_text(message->chat_id,
                  "The LLM provider is not configured. Check llm_provider, llm_model, llm_api_key and llm_endpoint.",
                  true);
        return false;
    }
    s_state = MP_AGENT_CONTEXT;
    s_trace_length = 0;
    s_tool_trace[0] = 0;
    if (!build_instructions(message->proactive) || !build_request(message)) {
        s_state = MP_AGENT_ERROR;
        send_text(message->chat_id, "The request exceeded the device buffer.", true);
        return false;
    }
    while (true) {
        s_state = MP_AGENT_INFERENCE;
        int64_t started = esp_timer_get_time();
        esp_err_t error = mp_llm_stream(s_request, &s_result);
        mp_metrics_inference((uint32_t)((esp_timer_get_time() - started) / 1000));
        if (error != ESP_OK) {
            s_state = MP_AGENT_ERROR;
            mp_metrics_error("llm", error);
            if (!message->proactive) {
                send_text(message->chat_id,
                          s_result.error[0] ? s_result.error : esp_err_to_name(error), true);
            }
            return false;
        }
        if (!s_result.call_count) {
            s_state = MP_AGENT_RESPONSE;
            esp_err_t delivery = send_text(message->chat_id, s_result.text, true);
            if (message->proactive && delivery == ESP_OK && s_flush) {
                delivery = s_flush(portMAX_DELAY);
            }
            if (!message->proactive) {
                size_t turn_size = 0;
                if (persist_turn(message, &s_result, &turn_size)) {
                    compact_context(turn_size >= MP_CONTEXT_BYTE_TRIGGER);
                } else {
                    ESP_LOGW(TAG, "context append failed");
                }
            }
            return delivery == ESP_OK;
        }
        if (s_result.text[0]) {
            send_progress(message->chat_id, s_result.text);
            if (!append_progress(&s_result)) {
                s_state = MP_AGENT_ERROR;
                send_text(message->chat_id, "The progress trace exceeded the device buffer.", true);
                return false;
            }
        }
        s_state = MP_AGENT_TOOL;
        mp_tool_context_t context = {0};
        strlcpy(context.chat_id, message->chat_id, sizeof(context.chat_id));
        size_t offset = 0;
        const mp_llm_call_t *call;
        while ((call = mp_llm_call_next(&s_result, &offset))) {
            s_tool_output[0] = 0;
            started = esp_timer_get_time();
            error = mp_tools_execute(call->name, call->arguments, &context, false,
                                     s_tool_output, sizeof(s_tool_output));
            mp_metrics_tool(call->name,
                            (uint32_t)((esp_timer_get_time() - started) / 1000));
            if (error != ESP_OK && !s_tool_output[0]) {
                snprintf(s_tool_output, sizeof(s_tool_output), "Tool error: %s",
                         esp_err_to_name(error));
            }
            if (!append_tool_exchange(call, s_tool_output)) {
                s_state = MP_AGENT_ERROR;
                send_text(message->chat_id, "The tool batch exceeded the trace buffer.", true);
                return false;
            }
        }
        if (!build_request(message)) {
            s_state = MP_AGENT_ERROR;
            send_text(message->chat_id, "The tool results exceeded the request buffer.", true);
            return false;
        }
    }
}

static bool handle_command(const mp_message_t *message)
{
    if (strcmp(message->text, "/help") == 0 || strcmp(message->text, "/start") == 0) {
        send_text(message->chat_id,
                  "MicroPaw is ready. Commands: /metrics, /memory, /jobs, /forget, /reset, /reset cancel, /update, /confirm ID, /cancel ID.",
                  true);
        return true;
    }
    if (strcmp(message->text, "/metrics") == 0) {
        mp_metrics_format(s_tool_output, sizeof(s_tool_output));
        send_text(message->chat_id, s_tool_output, true);
        return true;
    }
    if (strcmp(message->text, "/memory") == 0) {
        mp_memory_format(s_tool_output, sizeof(s_tool_output));
        send_text(message->chat_id, s_tool_output[0] ? s_tool_output : "No saved memory.", true);
        return true;
    }
    if (strcmp(message->text, "/jobs") == 0) {
        mp_scheduler_format(s_tool_output, sizeof(s_tool_output));
        send_text(message->chat_id, s_tool_output, true);
        return true;
    }
    if (strcmp(message->text, "/forget") == 0) {
        esp_err_t error = mp_context_forget();
        send_text(message->chat_id,
                  error == ESP_OK ? "Conversation context cleared. Saved facts and jobs remain." :
                  esp_err_to_name(error), true);
        return true;
    }
    if (strcmp(message->text, "/reset cancel") == 0) {
        if (!owner_message(message)) {
            send_text(message->chat_id, "Owner only.", true);
        } else {
            reset_confirmation();
            send_text(message->chat_id, "Reset cancelled.", true);
        }
        return true;
    }
    if (strcmp(message->text, "/reset") == 0) {
        if (!owner_message(message)) {
            send_text(message->chat_id, "Owner only.", true);
            return true;
        }
        s_reset.step = 1;
        s_reset.expires_us = esp_timer_get_time() + 300000000LL;
        strlcpy(s_reset.chat_id, message->chat_id, sizeof(s_reset.chat_id));
        send_text(message->chat_id,
                  "Reset removes saved facts, conversation context, jobs and pending permissions. Credentials, permission modes, timezone and model settings stay. Send /reset confirm within five minutes.",
                  true);
        return true;
    }
    if (strcmp(message->text, "/reset confirm") == 0) {
        if (!owner_message(message) || s_reset.step != 1 ||
            strcmp(s_reset.chat_id, message->chat_id) != 0 ||
            esp_timer_get_time() > s_reset.expires_us) {
            reset_confirmation();
            send_text(message->chat_id, "Reset confirmation is missing or expired.", true);
        } else {
            s_reset.step = 2;
            send_text(message->chat_id,
                      "Final warning: this cannot be undone on the device. Send /reset confirm YES before the five-minute window expires, or /reset cancel.",
                      true);
        }
        return true;
    }
    if (strcmp(message->text, "/reset confirm YES") == 0) {
        if (!owner_message(message) || s_reset.step != 2 ||
            strcmp(s_reset.chat_id, message->chat_id) != 0 ||
            esp_timer_get_time() > s_reset.expires_us) {
            reset_confirmation();
            send_text(message->chat_id, "Reset confirmation is missing or expired.", true);
        } else {
            esp_err_t error = reset_state();
            send_text(message->chat_id,
                      error == ESP_OK ? "MicroPaw is ready with a fresh state." :
                      esp_err_to_name(error), true);
        }
        return true;
    }
    if (strcmp(message->text, "/update") == 0) {
        if (!owner_message(message)) {
            send_text(message->chat_id, "Owner only.", true);
            return true;
        }
        s_tool_output[0] = 0;
        esp_err_t error = mp_ota_update(ota_progress, (void *)message->chat_id,
                                        s_tool_output, sizeof(s_tool_output));
        send_text(message->chat_id,
                  s_tool_output[0] ? s_tool_output : esp_err_to_name(error), true);
        if (error == ESP_OK) {
            if (s_flush) {
                s_flush(pdMS_TO_TICKS(30000));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        return true;
    }
    unsigned long parsed;
    if (sscanf(message->text, "/confirm %lu", &parsed) == 1 && parsed <= UINT32_MAX) {
        execute_confirmation(message, (uint32_t)parsed);
        return true;
    }
    if (sscanf(message->text, "/cancel %lu", &parsed) == 1 && parsed <= UINT32_MAX) {
        esp_err_t error = mp_confirmation_cancel(message->chat_id, (uint32_t)parsed);
        send_text(message->chat_id,
                  error == ESP_OK ? "Permission cancelled." :
                  "Permission not found or expired.", true);
        return true;
    }
    return false;
}

static bool owner_message(const mp_message_t *message)
{
    const char *owner = mp_config_get()->owner_chat_id;
    return owner[0] && strcmp(message->chat_id, owner) == 0;
}

static void reset_confirmation(void)
{
    memset(&s_reset, 0, sizeof(s_reset));
}

void mp_agent_reset_confirmation(void)
{
    reset_confirmation();
}

static esp_err_t reset_state(void)
{
    esp_err_t error = mp_memory_reset();
    if (error == ESP_OK) {
        error = mp_context_forget();
    }
    if (error == ESP_OK) {
        error = mp_scheduler_reset();
    }
    if (error == ESP_OK) {
        mp_confirmation_reset();
    }
    reset_confirmation();
    return error;
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

static bool append_input_message(mp_writer_t *writer, const char *role,
                                 const char *text, const char *id)
{
    mp_writer_raw(writer, "{\"type\":\"message\",\"role\":");
    mp_writer_string(writer, role);
    if (strcmp(role, "assistant") == 0) {
        if (id && id[0]) {
            mp_writer_raw(writer, ",\"id\":");
            mp_writer_string(writer, id);
        }
        mp_writer_raw(writer,
                      ",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":");
        mp_writer_string(writer, text);
        mp_writer_raw(writer, ",\"annotations\":[]}]}");
    } else {
        mp_writer_raw(writer, ",\"content\":[{\"type\":\"input_text\",\"text\":");
        mp_writer_string(writer, text);
        mp_writer_raw(writer, "}]}");
    }
    return writer->valid;
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
    if (strcmp(config->llm_provider, "openrouter") == 0) {
        mp_writer_raw(&writer, ",\"session_id\":");
        mp_writer_string(&writer, mp_llm_boot_id());
    } else if (strcmp(config->llm_provider, "openai") == 0) {
        mp_writer_raw(&writer, ",\"prompt_cache_key\":");
        mp_writer_string(&writer, mp_llm_boot_id());
    }
    mp_writer_raw(&writer, ",\"instructions\":");
    mp_writer_string(&writer, s_instructions);
    mp_writer_raw(&writer, ",\"input\":[");
    bool comma = false;
    if (!message->proactive) {
        comma = mp_context_format(&writer);
        if (!writer.valid) {
            return false;
        }
    }
    if (comma) {
        mp_writer_char(&writer, ',');
    }
    append_input_message(&writer, "user", message->text, NULL);
    if (s_trace_length) {
        mp_writer_char(&writer, ',');
        mp_writer_raw(&writer, s_tool_trace);
    }
    mp_writer_raw(&writer, "],\"tools\":");
    mp_writer_raw(&writer, mp_tools_catalog());
    mp_writer_format(&writer,
                     ",\"parallel_tool_calls\":true,\"stream\":true,\"store\":false,\"max_output_tokens\":%lu}",
                     strtoul(config->llm_max_output_tokens, NULL, 10));
    return writer.valid;
}

static bool append_progress(const mp_llm_result_t *result)
{
    mp_writer_t writer;
    mp_writer_init(&writer, s_tool_trace + s_trace_length,
                   sizeof(s_tool_trace) - s_trace_length);
    if (s_trace_length) {
        mp_writer_char(&writer, ',');
    }
    append_input_message(&writer, "assistant", result->text, result->message_id);
    if (!writer.valid) {
        return false;
    }
    s_trace_length += writer.length;
    return true;
}

static bool append_tool_exchange(const mp_llm_call_t *call, const char *output)
{
    mp_writer_t writer;
    mp_writer_init(&writer, s_tool_trace + s_trace_length,
                   sizeof(s_tool_trace) - s_trace_length);
    if (s_trace_length) {
        mp_writer_char(&writer, ',');
    }
    mp_writer_raw(&writer, "{\"type\":\"function_call\",\"id\":");
    mp_writer_string(&writer, call->item_id);
    mp_writer_raw(&writer, ",\"call_id\":");
    mp_writer_string(&writer, call->call_id);
    mp_writer_raw(&writer, ",\"name\":");
    mp_writer_string(&writer, call->name);
    mp_writer_raw(&writer, ",\"arguments\":");
    mp_writer_string(&writer, call->arguments);
    mp_writer_raw(&writer, "},{\"type\":\"function_call_output\",\"call_id\":");
    mp_writer_string(&writer, call->call_id);
    mp_writer_raw(&writer, ",\"output\":");
    mp_writer_string(&writer, output);
    mp_writer_char(&writer, '}');
    if (!writer.valid) {
        return false;
    }
    s_trace_length += writer.length;
    return true;
}

static bool persist_turn(const mp_message_t *message, const mp_llm_result_t *final_result,
                         size_t *turn_size)
{
    mp_writer_t writer;
    mp_writer_init(&writer, s_request, sizeof(s_request));
    append_input_message(&writer, "user", message->text, NULL);
    if (s_trace_length) {
        mp_writer_char(&writer, ',');
        mp_writer_raw(&writer, s_tool_trace);
    }
    mp_writer_char(&writer, ',');
    append_input_message(&writer, "assistant", final_result->text, final_result->message_id);
    if (!writer.valid) {
        return false;
    }
    *turn_size = writer.length;
    return mp_context_append(s_request, writer.length) == ESP_OK;
}

static bool build_compaction_request(uint32_t remove_count)
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
    if (strcmp(config->llm_provider, "openrouter") == 0) {
        mp_writer_raw(&writer, ",\"session_id\":");
        mp_writer_string(&writer, mp_llm_boot_id());
    } else if (strcmp(config->llm_provider, "openai") == 0) {
        mp_writer_raw(&writer, ",\"prompt_cache_key\":");
        mp_writer_string(&writer, mp_llm_boot_id());
    }
    mp_writer_raw(
        &writer,
        ",\"instructions\":\"Rewrite the supplied conversation into one durable rolling summary. Preserve preferences, exact identifiers and dates, decisions, unfinished work, promises and meaningful tool outcomes. Return only the summary text.\",\"input\":[");
    if (!mp_context_format_compaction(&writer, remove_count)) {
        return false;
    }
    mp_writer_format(&writer,
                     "],\"parallel_tool_calls\":false,\"stream\":true,\"store\":false,\"max_output_tokens\":%lu}",
                     strtoul(config->llm_max_output_tokens, NULL, 10));
    return writer.valid;
}

static void compact_context(bool force)
{
    mp_context_compaction_t plan;
    uint32_t remove_count;
    if (force) {
        remove_count = mp_context_turn_count();
        if (!remove_count) {
            return;
        }
    } else {
        if (!mp_context_compaction_plan(&plan)) {
            return;
        }
        remove_count = plan.remove_count;
    }
    int64_t started = esp_timer_get_time();
    bool success = build_compaction_request(remove_count) &&
                   mp_llm_stream(s_request, &s_result) == ESP_OK &&
                   !s_result.call_count && s_result.text_length < MP_CONTEXT_SUMMARY_TEXT_LEN;
    if (success) {
        mp_writer_t writer;
        mp_writer_init(&writer, s_tool_trace, sizeof(s_tool_trace));
        mp_writer_raw(&writer,
                      "{\"type\":\"message\",\"role\":\"developer\",\"content\":[{\"type\":\"input_text\",\"text\":");
        mp_writer_string(&writer, s_result.text);
        mp_writer_raw(&writer, "}]}");
        success = writer.valid &&
                  mp_context_commit_summary(s_tool_trace, writer.length, remove_count,
                                            (uint32_t)((esp_timer_get_time() - started) / 1000)) ==
                  ESP_OK;
    }
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
    if (!success) {
        mp_context_compaction_failed(elapsed);
    }
}

static esp_err_t send_text(const char *chat_id, const char *text, bool final)
{
    esp_err_t error = ESP_OK;
    if (s_send) {
        error = s_send(chat_id, text);
    }
    if (final && s_finish) {
        s_finish(chat_id);
    }
    return error;
}

static void send_progress(const char *chat_id, const char *text)
{
    mp_metrics_progress();
    send_text(chat_id, text, false);
}

static void execute_confirmation(const mp_message_t *message, uint32_t id)
{
    esp_err_t error = mp_confirmation_take(message->chat_id, id, s_confirm_tool,
                                            sizeof(s_confirm_tool), s_confirm_arguments,
                                            sizeof(s_confirm_arguments));
    if (error != ESP_OK) {
        send_text(message->chat_id, "Permission not found or expired.", true);
        return;
    }
    mp_tool_context_t context = {0};
    strlcpy(context.chat_id, message->chat_id, sizeof(context.chat_id));
    s_tool_output[0] = 0;
    int64_t started = esp_timer_get_time();
    error = mp_tools_execute(s_confirm_tool, s_confirm_arguments, &context, true,
                             s_tool_output, sizeof(s_tool_output));
    mp_metrics_tool(s_confirm_tool, (uint32_t)((esp_timer_get_time() - started) / 1000));
    send_text(message->chat_id,
              error == ESP_OK || s_tool_output[0] ? s_tool_output : esp_err_to_name(error), true);
}

static void ota_progress(const char *text, void *context)
{
    send_progress(context, text);
}

esp_err_t mp_agent_init(mp_send_fn send, mp_flush_fn flush, mp_finish_fn finish)
{
    s_send = send;
    s_flush = flush;
    s_finish = finish;
    esp_err_t error = mp_llm_init();
    if (error != ESP_OK) {
        return error;
    }
    if (!mp_context_turn_count()) {
        size_t legacy_size = mp_history_export(s_tool_output, sizeof(s_tool_output));
        if (legacy_size && mp_context_append(s_tool_output, legacy_size) == ESP_OK) {
            mp_history_erase();
        }
    }
    s_queue = xQueueCreateStatic(AGENT_QUEUE_LENGTH, sizeof(mp_message_t),
                                 s_queue_storage, &s_queue_buffer);
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
    return submit_message(0, chat_id, text, proactive, timeout);
}

esp_err_t mp_agent_submit_scheduled(uint32_t id, const char *chat_id, const char *text)
{
    return id ? submit_message(id, chat_id, text, true, pdMS_TO_TICKS(100)) :
           ESP_ERR_INVALID_ARG;
}

static esp_err_t submit_message(uint32_t schedule_id, const char *chat_id, const char *text,
                                bool proactive, TickType_t timeout)
{
    if (!s_queue || !chat_id || !text || strlen(chat_id) >= MP_CHAT_ID_LEN ||
        strlen(text) >= MP_MESSAGE_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_message_t message = {
        .queued_us = esp_timer_get_time(),
        .schedule_id = schedule_id,
        .proactive = proactive
    };
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
