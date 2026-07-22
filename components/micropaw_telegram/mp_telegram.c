#include "mp_telegram.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "mp_agent.h"
#include "mp_config.h"
#include "mp_json.h"
#include "mp_metrics.h"
#include "mp_net.h"
#include "mp_wifi.h"
#include "sdkconfig.h"

#include "freertos/semphr.h"

EXT_RAM_BSS_ATTR static char s_response[6144];
EXT_RAM_BSS_ATTR static char s_body[4608];
EXT_RAM_BSS_ATTR static char s_part[3901];
EXT_RAM_BSS_ATTR static char s_message[MP_MESSAGE_LEN];
static StaticSemaphore_t s_send_lock_buffer;
static SemaphoreHandle_t s_send_lock;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[CONFIG_MICROPAW_TELEGRAM_STACK];
static TaskHandle_t s_task;
static int64_t s_offset;
static const char *TAG = "telegram";

static void telegram_task(void *argument);
static esp_err_t poll_once(void);
static void process_update(const char *update, size_t length);
static size_t utf8_chunk(const char *text, size_t limit);
static esp_err_t send_chunk(const char *chat_id, const char *text, size_t length);
esp_err_t mp_telegram_start(void);
esp_err_t mp_telegram_send(const char *chat_id, const char *text);
TaskHandle_t mp_telegram_task_handle(void);

static void telegram_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!mp_wifi_wait(portMAX_DELAY)) {
            continue;
        }
        esp_err_t error = poll_once();
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "poll: %s", esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

static esp_err_t poll_once(void)
{
    const mp_config_t *config = mp_config_get();
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/getUpdates?offset=%lld&limit=1&timeout=15&allowed_updates=%%5B%%22message%%22%%5D",
             config->telegram_token, (long long)s_offset);
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .response_limit = sizeof(s_response) - 1,
        .timeout_ms = 20000,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response;
    esp_err_t error = mp_http_collect(&request, s_response, sizeof(s_response), &response);
    if (error != ESP_OK || response.status != 200) {
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    bool ok;
    const char *results;
    size_t results_length;
    size_t response_length = strlen(s_response);
    if (!mp_json_get_bool(s_response, response_length, "ok", &ok) || !ok ||
        !mp_json_get_slice(s_response, response_length, "result", &results, &results_length)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *update;
    size_t update_length;
    if (mp_json_first(results, results_length, &update, &update_length)) {
        process_update(update, update_length);
    }
    return ESP_OK;
}

static void process_update(const char *update, size_t length)
{
    const mp_config_t *config = mp_config_get();
    int64_t update_id;
    if (mp_json_get_int64(update, length, "update_id", &update_id)) {
        s_offset = update_id + 1;
    }
    const char *message;
    const char *chat;
    size_t message_length;
    size_t chat_length;
    int64_t chat_id;
    if (!mp_json_get_slice(update, length, "message", &message, &message_length) ||
        !mp_json_get_slice(message, message_length, "chat", &chat, &chat_length) ||
        !mp_json_get_int64(chat, chat_length, "id", &chat_id)) {
        return;
    }
    char id[MP_CHAT_ID_LEN];
    snprintf(id, sizeof(id), "%lld", (long long)chat_id);
    if (strcmp(id, config->owner_chat_id) != 0) {
        return;
    }
    if (!mp_json_get_string(message, message_length, "text", s_message, sizeof(s_message))) {
        mp_telegram_send(id, "Message too long for the device input buffer.");
        return;
    }
    if (mp_agent_submit(id, s_message, false, 0) != ESP_OK) {
        mp_telegram_send(id, "MicroPaw is busy. Try again shortly.");
    }
}

static size_t utf8_chunk(const char *text, size_t limit)
{
    size_t length = strlen(text);
    if (length <= limit) {
        return length;
    }
    size_t split = limit;
    while (split && ((unsigned char)text[split] & 0xc0) == 0x80) {
        split--;
    }
    for (size_t index = split; index > split / 2; index--) {
        if (text[index] == '\n' || text[index] == ' ') {
            return index;
        }
    }
    return split;
}

static esp_err_t send_chunk(const char *chat_id, const char *text, size_t length)
{
    const mp_config_t *config = mp_config_get();
    memcpy(s_part, text, length);
    s_part[length] = 0;
    mp_writer_t writer;
    mp_writer_init(&writer, s_body, sizeof(s_body));
    mp_writer_raw(&writer, "{\"chat_id\":");
    mp_writer_string(&writer, chat_id);
    mp_writer_raw(&writer, ",\"text\":");
    mp_writer_string(&writer, s_part);
    mp_writer_char(&writer, '}');
    if (!writer.valid) {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", config->telegram_token);
    mp_http_header_t headers[] = {{"Content-Type", "application/json"}};
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .headers = headers,
        .header_count = 1,
        .body = s_body,
        .body_size = writer.length,
        .response_limit = sizeof(s_response) - 1,
        .timeout_ms = 20000,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response;
    esp_err_t error = mp_http_collect(&request, s_response, sizeof(s_response), &response);
    return error == ESP_OK && response.status == 200 ? ESP_OK :
           error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
}

esp_err_t mp_telegram_start(void)
{
    const mp_config_t *config = mp_config_get();
    if (!config->telegram_token[0] || !config->owner_chat_id[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_send_lock) {
        s_send_lock = xSemaphoreCreateMutexStatic(&s_send_lock_buffer);
    }
    if (s_task) {
        return ESP_OK;
    }
    s_task = xTaskCreateStaticPinnedToCore(telegram_task, "mp_telegram",
                                           CONFIG_MICROPAW_TELEGRAM_STACK, NULL, 4,
                                           s_task_stack, &s_task_buffer, 0);
    if (s_task) {
        mp_metrics_register("telegram", s_task);
    }
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mp_telegram_send(const char *chat_id, const char *text)
{
    if (!chat_id || !text || !chat_id[0] || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_send_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    while (*text) {
        size_t length = utf8_chunk(text, 3900);
        esp_err_t error = send_chunk(chat_id, text, length);
        if (error != ESP_OK) {
            xSemaphoreGive(s_send_lock);
            return error;
        }
        text += length;
        while (*text == ' ' || *text == '\n') {
            text++;
        }
    }
    xSemaphoreGive(s_send_lock);
    return ESP_OK;
}

TaskHandle_t mp_telegram_task_handle(void)
{
    return s_task;
}
