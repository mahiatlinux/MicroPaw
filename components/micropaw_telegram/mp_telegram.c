#include "mp_telegram.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mp_agent.h"
#include "mp_config.h"
#include "mp_display.h"
#include "mp_json.h"
#include "mp_metrics.h"
#include "mp_net.h"
#include "mp_wifi.h"
#include "sdkconfig.h"

#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define OUTBOUND_QUEUE_LENGTH 4
#define OUTBOUND_IDLE BIT0

typedef enum {
    OUTBOUND_TEXT,
    OUTBOUND_TYPING_START,
    OUTBOUND_TYPING_STOP
} outbound_type_t;

typedef struct {
    outbound_type_t type;
    uint16_t text_length;
    int64_t queued_us;
    char chat_id[MP_CHAT_ID_LEN];
    char text[3901];
} outbound_item_t;

EXT_RAM_BSS_ATTR static char s_poll_response[6144];
EXT_RAM_BSS_ATTR static char s_message[MP_MESSAGE_LEN];
EXT_RAM_BSS_ATTR static char s_body[24576];
EXT_RAM_BSS_ATTR static char s_part[3901];
EXT_RAM_BSS_ATTR static outbound_item_t s_enqueue;
EXT_RAM_BSS_ATTR static outbound_item_t s_current;
EXT_RAM_BSS_ATTR static uint8_t s_outbound_storage[OUTBOUND_QUEUE_LENGTH *
                                                   sizeof(outbound_item_t)];
static StaticQueue_t s_outbound_queue_buffer;
static QueueHandle_t s_outbound_queue;
static StaticSemaphore_t s_enqueue_mutex_buffer;
static SemaphoreHandle_t s_enqueue_mutex;
static StaticEventGroup_t s_outbound_event_buffer;
static EventGroupHandle_t s_outbound_event;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_pending;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[CONFIG_MICROPAW_TELEGRAM_STACK];
static TaskHandle_t s_task;
static StaticTask_t s_sender_task_buffer;
static StackType_t s_sender_task_stack[CONFIG_MICROPAW_TELEGRAM_SENDER_STACK];
static TaskHandle_t s_sender_task;
static mp_http_session_t s_poll_session;
static mp_http_session_t s_sender_session;
static int64_t s_offset;
static esp_err_t s_delivery_error;
static int s_send_status;
static const char *TAG = "telegram";

static void telegram_task(void *argument);
static void sender_task(void *argument);
static esp_err_t poll_once(void);
static void process_update(const char *update, size_t length);
static size_t utf8_chunk(const char *text, size_t length, size_t limit);
static esp_err_t send_request(const char *method, const char *chat_id,
                              const char *text, size_t length);
static esp_err_t send_chunk(const char *chat_id, const char *text, size_t length);
static esp_err_t send_typing(const char *chat_id);
static esp_err_t deliver_text(const char *chat_id, const char *text, size_t length);
static void pending_add(void);
static void pending_complete(void);
static esp_err_t queue_item(TickType_t timeout);
esp_err_t mp_telegram_start(void);
esp_err_t mp_telegram_send(const char *chat_id, const char *text);
esp_err_t mp_telegram_typing_start(const char *chat_id, int64_t queued_us);
void mp_telegram_typing_stop(const char *chat_id);
esp_err_t mp_telegram_flush(TickType_t timeout);
TaskHandle_t mp_telegram_task_handle(void);
TaskHandle_t mp_telegram_sender_task_handle(void);

static void telegram_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!mp_wifi_wait(portMAX_DELAY)) {
            continue;
        }
        esp_err_t error = poll_once();
        if (error != ESP_OK) {
            mp_metrics_error("telegram_poll", error);
            ESP_LOGW(TAG, "poll: %s", esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

static void sender_task(void *argument)
{
    (void)argument;
    bool typing = false;
    char typing_chat[MP_CHAT_ID_LEN] = "";
    while (true) {
        TickType_t timeout = typing ? pdMS_TO_TICKS(4000) : portMAX_DELAY;
        if (xQueueReceive(s_outbound_queue, &s_current, timeout) == pdTRUE) {
            if (s_current.type == OUTBOUND_TYPING_START) {
                typing = true;
                strlcpy(typing_chat, s_current.chat_id, sizeof(typing_chat));
                mp_metrics_typing(s_current.queued_us);
                if (mp_wifi_wait(portMAX_DELAY)) {
                    esp_err_t error = send_typing(typing_chat);
                    if (error != ESP_OK) {
                        mp_metrics_error("telegram_typing", error);
                    }
                }
            } else if (s_current.type == OUTBOUND_TYPING_STOP) {
                if (strcmp(typing_chat, s_current.chat_id) == 0) {
                    typing = false;
                    typing_chat[0] = 0;
                    mp_display_response_end();
                }
            } else {
                esp_err_t error = deliver_text(s_current.chat_id, s_current.text,
                                               s_current.text_length);
                mp_metrics_delivery(
                    (uint32_t)((esp_timer_get_time() - s_current.queued_us) / 1000));
                if (error != ESP_OK) {
                    portENTER_CRITICAL(&s_pending_lock);
                    s_delivery_error = error;
                    portEXIT_CRITICAL(&s_pending_lock);
                    ESP_LOGW(TAG, "send: %s", esp_err_to_name(error));
                }
            }
            pending_complete();
        } else if (typing && mp_wifi_wait(0)) {
            esp_err_t error = send_typing(typing_chat);
            if (error != ESP_OK) {
                mp_metrics_error("telegram_typing", error);
            }
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
        .response_limit = sizeof(s_poll_response) - 1,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response;
    esp_err_t error = mp_http_session_collect(&s_poll_session, &request, s_poll_response,
                                              sizeof(s_poll_response), &response);
    if (error != ESP_OK || response.status != 200) {
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    bool ok;
    const char *results;
    size_t results_length;
    size_t response_length = strlen(s_poll_response);
    if (!mp_json_get_bool(s_poll_response, response_length, "ok", &ok) || !ok ||
        !mp_json_get_slice(s_poll_response, response_length, "result", &results, &results_length)) {
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
        mp_telegram_typing_stop(id);
        return;
    }
    int64_t queued_us = esp_timer_get_time();
    mp_telegram_typing_start(id, queued_us);
    if (mp_agent_submit(id, s_message, false, portMAX_DELAY) != ESP_OK) {
        mp_telegram_send(id, "MicroPaw is busy. Try again shortly.");
        mp_telegram_typing_stop(id);
    }
}

static size_t utf8_chunk(const char *text, size_t length, size_t limit)
{
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

static esp_err_t send_request(const char *method, const char *chat_id,
                              const char *text, size_t length)
{
    s_send_status = 0;
    const mp_config_t *config = mp_config_get();
    mp_writer_t writer;
    mp_writer_init(&writer, s_body, sizeof(s_body));
    mp_writer_raw(&writer, "{\"chat_id\":");
    mp_writer_string(&writer, chat_id);
    if (text) {
        memcpy(s_part, text, length);
        s_part[length] = 0;
        mp_writer_raw(&writer, ",\"text\":");
        mp_writer_string(&writer, s_part);
    } else {
        mp_writer_raw(&writer, ",\"action\":\"typing\"");
    }
    mp_writer_char(&writer, '}');
    if (!writer.valid) {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s",
             config->telegram_token, method);
    mp_http_header_t headers[] = {{"Content-Type", "application/json"}};
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .headers = headers,
        .header_count = 1,
        .body = s_body,
        .body_size = writer.length,
        .response_limit = 512,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response;
    esp_err_t error = mp_http_session_stream(&s_sender_session, &request, NULL, NULL, &response);
    s_send_status = response.status;
    return error == ESP_OK && response.status == 200 ? ESP_OK :
           error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
}

static esp_err_t send_chunk(const char *chat_id, const char *text, size_t length)
{
    return send_request("sendMessage", chat_id, text, length);
}

static esp_err_t send_typing(const char *chat_id)
{
    return send_request("sendChatAction", chat_id, NULL, 0);
}

static esp_err_t deliver_text(const char *chat_id, const char *text, size_t length)
{
    while (true) {
        if (!mp_wifi_wait(portMAX_DELAY)) {
            continue;
        }
        esp_err_t error = send_chunk(chat_id, text, length);
        if (error == ESP_OK) {
            return error;
        }
        mp_metrics_error("telegram_send", error);
        if (!mp_http_retryable(error) && !mp_http_status_retryable(s_send_status)) {
            return error;
        }
        ESP_LOGW(TAG, "send retry: %s", esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void pending_add(void)
{
    portENTER_CRITICAL(&s_pending_lock);
    bool first = s_pending++ == 0;
    if (first) {
        s_delivery_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_pending_lock);
    if (first) {
        xEventGroupClearBits(s_outbound_event, OUTBOUND_IDLE);
    }
}

static void pending_complete(void)
{
    portENTER_CRITICAL(&s_pending_lock);
    if (s_pending) {
        s_pending--;
    }
    bool idle = s_pending == 0;
    portEXIT_CRITICAL(&s_pending_lock);
    if (idle) {
        xEventGroupSetBits(s_outbound_event, OUTBOUND_IDLE);
    }
}

static esp_err_t queue_item(TickType_t timeout)
{
    pending_add();
    if (xQueueSend(s_outbound_queue, &s_enqueue, timeout) != pdTRUE) {
        pending_complete();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mp_telegram_start(void)
{
    const mp_config_t *config = mp_config_get();
    if (!config->telegram_token[0] || !config->owner_chat_id[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_outbound_queue) {
        s_outbound_queue = xQueueCreateStatic(OUTBOUND_QUEUE_LENGTH, sizeof(outbound_item_t),
                                              s_outbound_storage,
                                              &s_outbound_queue_buffer);
        s_outbound_event = xEventGroupCreateStatic(&s_outbound_event_buffer);
        s_enqueue_mutex = xSemaphoreCreateMutexStatic(&s_enqueue_mutex_buffer);
        if (!s_outbound_queue || !s_outbound_event || !s_enqueue_mutex) {
            return ESP_ERR_NO_MEM;
        }
        xEventGroupSetBits(s_outbound_event, OUTBOUND_IDLE);
    }
    if (!s_sender_task) {
        s_sender_task = xTaskCreateStaticPinnedToCore(
            sender_task, "mp_tg_send", CONFIG_MICROPAW_TELEGRAM_SENDER_STACK, NULL, 4,
            s_sender_task_stack, &s_sender_task_buffer, 0);
        if (!s_sender_task) {
            return ESP_ERR_NO_MEM;
        }
        mp_metrics_register("telegram_sender", s_sender_task);
    }
    if (!s_task) {
        s_task = xTaskCreateStaticPinnedToCore(telegram_task, "mp_telegram",
                                               CONFIG_MICROPAW_TELEGRAM_STACK, NULL, 4,
                                               s_task_stack, &s_task_buffer, 0);
        if (s_task) {
            mp_metrics_register("telegram", s_task);
        }
    }
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mp_telegram_send(const char *chat_id, const char *text)
{
    if (!s_outbound_queue || !chat_id || !text || !chat_id[0] || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_enqueue_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = ESP_OK;
    size_t remaining = strlen(text);
    while (remaining) {
        size_t length = utf8_chunk(text, remaining, 3900);
        memset(&s_enqueue, 0, sizeof(s_enqueue));
        s_enqueue.type = OUTBOUND_TEXT;
        s_enqueue.text_length = length;
        s_enqueue.queued_us = esp_timer_get_time();
        strlcpy(s_enqueue.chat_id, chat_id, sizeof(s_enqueue.chat_id));
        memcpy(s_enqueue.text, text, length);
        s_enqueue.text[length] = 0;
        error = queue_item(portMAX_DELAY);
        if (error != ESP_OK) {
            break;
        }
        text += length;
        remaining -= length;
    }
    xSemaphoreGive(s_enqueue_mutex);
    return error;
}

esp_err_t mp_telegram_typing_start(const char *chat_id, int64_t queued_us)
{
    if (!s_outbound_queue || !chat_id || !chat_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_enqueue_mutex, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memset(&s_enqueue, 0, sizeof(s_enqueue));
    s_enqueue.type = OUTBOUND_TYPING_START;
    s_enqueue.queued_us = queued_us;
    strlcpy(s_enqueue.chat_id, chat_id, sizeof(s_enqueue.chat_id));
    esp_err_t error = queue_item(0);
    xSemaphoreGive(s_enqueue_mutex);
    return error;
}

void mp_telegram_typing_stop(const char *chat_id)
{
    if (!s_outbound_queue || !chat_id || !chat_id[0]) {
        return;
    }
    if (xSemaphoreTake(s_enqueue_mutex, portMAX_DELAY) == pdTRUE) {
        memset(&s_enqueue, 0, sizeof(s_enqueue));
        s_enqueue.type = OUTBOUND_TYPING_STOP;
        strlcpy(s_enqueue.chat_id, chat_id, sizeof(s_enqueue.chat_id));
        queue_item(portMAX_DELAY);
        xSemaphoreGive(s_enqueue_mutex);
    }
}

esp_err_t mp_telegram_flush(TickType_t timeout)
{
    if (!s_outbound_event) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_outbound_event, OUTBOUND_IDLE,
                                           pdFALSE, pdTRUE, timeout);
    portENTER_CRITICAL(&s_pending_lock);
    esp_err_t error = s_delivery_error;
    portEXIT_CRITICAL(&s_pending_lock);
    return bits & OUTBOUND_IDLE ? error : ESP_ERR_TIMEOUT;
}

TaskHandle_t mp_telegram_task_handle(void)
{
    return s_task;
}

TaskHandle_t mp_telegram_sender_task_handle(void)
{
    return s_sender_task;
}
