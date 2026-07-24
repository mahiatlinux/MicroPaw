#include "mp_instagram.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mp_agent.h"
#include "mp_config.h"
#include "mp_json.h"
#include "mp_metrics.h"
#include "mp_net.h"
#include "mp_types.h"
#include "mp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define CHAT_PREFIX "ig:"
#define OUTBOUND_QUEUE_LENGTH 4
#define OUTBOUND_IDLE BIT0
#define OUTBOUND_TEXT_LEN 1001
#define MESSAGE_PAGE_SIZE 50
#define MESSAGE_PAGE_COUNT 32
#define CURSOR_LEN 96
#define POLL_INTERVAL_MS 1250

typedef struct {
    uint16_t text_length;
    int64_t queued_us;
    char chat_id[MP_CHAT_ID_LEN];
    char text[OUTBOUND_TEXT_LEN];
} outbound_item_t;

typedef struct {
    const char *data;
    size_t length;
} message_ref_t;

typedef struct {
    char poll_response[CONFIG_MICROPAW_WORK_TEXT_BYTES];
    char sender_body[2048];
    char message[MP_MESSAGE_LEN];
    char media_refs[MP_MEDIA_REF_ARENA_LEN];
    outbound_item_t enqueue;
    outbound_item_t current;
    uint8_t outbound_storage[OUTBOUND_QUEUE_LENGTH * sizeof(outbound_item_t)];
    char page_cursors[MESSAGE_PAGE_COUNT][CURSOR_LEN];
    char account_id[32];
    char conversation_id[MP_CHAT_ID_LEN - sizeof(CHAT_PREFIX) + 1];
    char owner_id[32];
    char last_message_id[256];
} instagram_memory_t;

static instagram_memory_t *s_memory;
static StaticQueue_t s_outbound_queue_buffer;
static QueueHandle_t s_outbound_queue;
static StaticSemaphore_t s_enqueue_mutex_buffer;
static SemaphoreHandle_t s_enqueue_mutex;
static StaticSemaphore_t s_http_mutex_buffer;
static SemaphoreHandle_t s_http_mutex;
static StaticEventGroup_t s_outbound_event_buffer;
static EventGroupHandle_t s_outbound_event;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_pending;
static StaticTask_t s_task_buffer;
static StackType_t *s_task_stack;
static TaskHandle_t s_task;
static StaticTask_t s_sender_task_buffer;
static StackType_t *s_sender_task_stack;
static TaskHandle_t s_sender_task;
static mp_http_session_t s_session;
static bool s_checkpoint_ready;
static esp_err_t s_delivery_error;
static int s_send_status;
static const char *TAG = "instagram";

static void instagram_task(void *argument);
static void sender_task(void *argument);
static esp_err_t checkpoint_init(void);
static esp_err_t owner_save(const char *participant_id);
static esp_err_t checkpoint_save(const char *message_id);
static esp_err_t checkpoint_ready(void);
static esp_err_t poll_once(void);
static esp_err_t find_conversation(void);
static esp_err_t fetch_messages(const char *cursor, bool *has_more, char *next_cursor,
                                size_t cursor_size);
static esp_err_t sync_messages(void);
static esp_err_t baseline_messages(void);
static esp_err_t find_checkpoint_page(size_t *page, size_t *checkpoint);
static esp_err_t process_page(size_t limit);
static esp_err_t collect_messages(message_ref_t *messages, size_t size, size_t *count);
static esp_err_t process_message(const char *json, size_t length);
static esp_err_t collect_image_refs(const char *json, size_t length, uint8_t *count,
                                    size_t *used);
static size_t utf8_chunk(const char *text, size_t length, size_t limit);
static esp_err_t send_chunk(const char *chat_id, const char *text, size_t length);
static esp_err_t deliver_text(const char *chat_id, const char *text, size_t length);
static void pending_add(void);
static void pending_complete(void);
static esp_err_t queue_item(TickType_t timeout);
static esp_err_t allocate_memory(void);
esp_err_t mp_instagram_start(void);
esp_err_t mp_instagram_send(const char *chat_id, const char *text);
esp_err_t mp_instagram_flush(TickType_t timeout);
bool mp_instagram_chat(const char *chat_id);
TaskHandle_t mp_instagram_task_handle(void);
TaskHandle_t mp_instagram_sender_task_handle(void);

static void instagram_task(void *argument)
{
    (void)argument;
    esp_err_t error = checkpoint_init();
    if (error != ESP_OK) {
        mp_metrics_error("instagram_state", error);
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        if (!mp_wifi_wait(portMAX_DELAY)) {
            continue;
        }
        int64_t started = esp_timer_get_time();
        error = poll_once();
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
            mp_metrics_error("instagram_poll", error);
            ESP_LOGW(TAG, "poll: %s", esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
            vTaskDelay(pdMS_TO_TICKS(elapsed < POLL_INTERVAL_MS ?
                                     POLL_INTERVAL_MS - elapsed : 250));
        }
    }
}

static void sender_task(void *argument)
{
    (void)argument;
    while (true) {
        if (xQueueReceive(s_outbound_queue, &s_memory->current, portMAX_DELAY) == pdTRUE) {
            esp_err_t error = deliver_text(s_memory->current.chat_id, s_memory->current.text,
                                           s_memory->current.text_length);
            mp_metrics_delivery(
                (uint32_t)((esp_timer_get_time() - s_memory->current.queued_us) / 1000));
            if (error != ESP_OK) {
                portENTER_CRITICAL(&s_pending_lock);
                s_delivery_error = error;
                portEXIT_CRITICAL(&s_pending_lock);
                ESP_LOGW(TAG, "send: %s", esp_err_to_name(error));
            }
            pending_complete();
        }
    }
}

static esp_err_t checkpoint_init(void)
{
    const char *owner = mp_config_get()->instagram_owner_username;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("instagram", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    char stored_owner[32] = "";
    size_t owner_size = sizeof(stored_owner);
    error = nvs_get_str(handle, "owner", stored_owner, &owner_size);
    if (error == ESP_ERR_NVS_NOT_FOUND || strcasecmp(stored_owner, owner) != 0) {
        error = nvs_set_str(handle, "owner", owner);
        if (error == ESP_OK) {
            error = nvs_erase_key(handle, "last_in");
        }
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
        if (error == ESP_OK) {
            error = nvs_erase_key(handle, "ready");
        }
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
        if (error == ESP_OK) {
            error = nvs_erase_key(handle, "participant");
        }
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
    } else if (error == ESP_OK) {
        size_t id_size = sizeof(s_memory->last_message_id);
        error = nvs_get_str(handle, "last_in", s_memory->last_message_id, &id_size);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
        uint8_t ready = 0;
        if (error == ESP_OK) {
            esp_err_t ready_error = nvs_get_u8(handle, "ready", &ready);
            if (ready_error != ESP_OK && ready_error != ESP_ERR_NVS_NOT_FOUND) {
                error = ready_error;
            }
        }
        s_checkpoint_ready = ready != 0;
        size_t participant_size = sizeof(s_memory->owner_id);
        if (error == ESP_OK) {
            esp_err_t participant_error = nvs_get_str(handle, "participant", s_memory->owner_id,
                                                      &participant_size);
            if (participant_error != ESP_OK &&
                participant_error != ESP_ERR_NVS_NOT_FOUND) {
                error = participant_error;
            }
        }
    }
    nvs_close(handle);
    return error;
}

static esp_err_t owner_save(const char *participant_id)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("instagram", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, "participant", participant_id);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error == ESP_OK) {
        strlcpy(s_memory->owner_id, participant_id, sizeof(s_memory->owner_id));
    }
    return error;
}

static esp_err_t checkpoint_save(const char *message_id)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("instagram", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, "last_in", message_id);
    if (error == ESP_OK) {
        error = nvs_set_u8(handle, "ready", 1);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error == ESP_OK) {
        strlcpy(s_memory->last_message_id, message_id, sizeof(s_memory->last_message_id));
        s_checkpoint_ready = true;
    }
    return error;
}

static esp_err_t checkpoint_ready(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("instagram", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_u8(handle, "ready", 1);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error == ESP_OK) {
        s_checkpoint_ready = true;
    }
    return error;
}

static esp_err_t poll_once(void)
{
    if (!s_memory->conversation_id[0]) {
        esp_err_t error = find_conversation();
        if (error != ESP_OK) {
            return error;
        }
    }
    return s_checkpoint_ready ? sync_messages() : baseline_messages();
}

static esp_err_t find_conversation(void)
{
    char cursor[CURSOR_LEN] = "";
    while (true) {
        char encoded[CURSOR_LEN * 3];
        mp_url_encode(cursor, encoded, sizeof(encoded));
        char url[512];
        snprintf(url, sizeof(url),
                 "https://zernio.com/api/v1/inbox/conversations?platform=instagram&status=active&sortOrder=desc&limit=100%s%s",
                 cursor[0] ? "&cursor=" : "", encoded);
        char authorization[112];
        snprintf(authorization, sizeof(authorization), "Bearer %s",
                 mp_config_get()->zernio_api_key);
        mp_http_header_t headers[] = {{"Authorization", authorization}};
        mp_http_request_t request = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .headers = headers,
            .header_count = 1,
            .response_limit = sizeof(s_memory->poll_response) - 1,
            .timeout_ms = 20000,
            .buffer_size = 1024,
            .accepted_content_types = "application/json"
        };
        mp_http_response_t response;
        xSemaphoreTake(s_http_mutex, portMAX_DELAY);
        esp_err_t error = mp_http_session_collect(&s_session, &request, s_memory->poll_response,
                                                  sizeof(s_memory->poll_response), &response);
        xSemaphoreGive(s_http_mutex);
        if (error != ESP_OK || response.status != 200 || response.truncated) {
            return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
        }
        const char *data;
        size_t data_length;
        if (!mp_json_get_slice(s_memory->poll_response, strlen(s_memory->poll_response), "data",
                               &data, &data_length)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t offset = 0;
        const char *item;
        size_t item_length;
        while (mp_json_next(data, data_length, &offset, &item, &item_length)) {
            char username[32];
            char participant_id[sizeof(s_memory->owner_id)];
            if (mp_json_get_string(item, item_length, "participantUsername",
                                   username, sizeof(username)) &&
                mp_json_get_string(item, item_length, "participantId",
                                   participant_id, sizeof(participant_id)) &&
                (s_memory->owner_id[0] ? strcmp(participant_id, s_memory->owner_id) == 0 :
                 strcasecmp(username, mp_config_get()->instagram_owner_username) == 0) &&
                mp_json_get_string(item, item_length, "accountId",
                                   s_memory->account_id, sizeof(s_memory->account_id)) &&
                mp_json_get_string(item, item_length, "id",
                                   s_memory->conversation_id, sizeof(s_memory->conversation_id))) {
                return s_memory->owner_id[0] ? ESP_OK : owner_save(participant_id);
            }
        }
        const char *pagination;
        size_t pagination_length;
        bool has_more = false;
        if (!mp_json_get_slice(s_memory->poll_response, strlen(s_memory->poll_response), "pagination",
                               &pagination, &pagination_length) ||
            !mp_json_get_bool(pagination, pagination_length, "hasMore", &has_more) ||
            !has_more ||
            !mp_json_get_string(pagination, pagination_length, "nextCursor",
                                cursor, sizeof(cursor))) {
            return ESP_ERR_NOT_FOUND;
        }
    }
}

static esp_err_t fetch_messages(const char *cursor, bool *has_more, char *next_cursor,
                                size_t cursor_size)
{
    char encoded[CURSOR_LEN * 3];
    mp_url_encode(cursor, encoded, sizeof(encoded));
    char url[512];
    snprintf(url, sizeof(url),
             "https://zernio.com/api/v1/inbox/conversations/%s/messages?accountId=%s&sortOrder=desc&limit=%u%s%s",
             s_memory->conversation_id, s_memory->account_id, MESSAGE_PAGE_SIZE,
             cursor[0] ? "&cursor=" : "", encoded);
    char authorization[112];
    snprintf(authorization, sizeof(authorization), "Bearer %s",
             mp_config_get()->zernio_api_key);
    mp_http_header_t headers[] = {{"Authorization", authorization}};
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .headers = headers,
        .header_count = 1,
        .response_limit = sizeof(s_memory->poll_response) - 1,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response;
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    esp_err_t error = mp_http_session_collect(&s_session, &request, s_memory->poll_response,
                                              sizeof(s_memory->poll_response), &response);
    xSemaphoreGive(s_http_mutex);
    if (error != ESP_OK || response.status != 200 || response.truncated) {
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    const char *pagination;
    size_t pagination_length;
    if (!mp_json_get_slice(s_memory->poll_response, strlen(s_memory->poll_response), "pagination",
                           &pagination, &pagination_length) ||
        !mp_json_get_bool(pagination, pagination_length, "hasMore", has_more)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    next_cursor[0] = 0;
    if (*has_more &&
        !mp_json_get_string(pagination, pagination_length, "nextCursor",
                            next_cursor, cursor_size)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t sync_messages(void)
{
    size_t page;
    size_t checkpoint;
    esp_err_t error = find_checkpoint_page(&page, &checkpoint);
    if (error != ESP_OK) {
        return error;
    }
    error = process_page(checkpoint);
    while (error == ESP_OK && page > 0) {
        page--;
        bool has_more;
        char next[CURSOR_LEN];
        error = fetch_messages(s_memory->page_cursors[page], &has_more, next, sizeof(next));
        if (error == ESP_OK) {
            error = process_page(SIZE_MAX);
        }
    }
    return error;
}

static esp_err_t baseline_messages(void)
{
    bool has_more;
    char next[CURSOR_LEN];
    esp_err_t error = fetch_messages("", &has_more, next, sizeof(next));
    if (error != ESP_OK) {
        return error;
    }
    message_ref_t messages[MESSAGE_PAGE_SIZE];
    size_t count;
    error = collect_messages(messages, MESSAGE_PAGE_SIZE, &count);
    if (error != ESP_OK) {
        return error;
    }
    for (size_t index = 0; index < count; index++) {
        char direction[12];
        char message_id[sizeof(s_memory->last_message_id)];
        if (mp_json_get_string(messages[index].data, messages[index].length,
                               "direction", direction, sizeof(direction)) &&
            strcmp(direction, "incoming") == 0 &&
            mp_json_get_string(messages[index].data, messages[index].length,
                               "id", message_id, sizeof(message_id))) {
            return checkpoint_save(message_id);
        }
    }
    return checkpoint_ready();
}

static esp_err_t find_checkpoint_page(size_t *page, size_t *checkpoint)
{
    s_memory->page_cursors[0][0] = 0;
    for (*page = 0; *page < MESSAGE_PAGE_COUNT; (*page)++) {
        bool has_more;
        char next[CURSOR_LEN];
        esp_err_t error = fetch_messages(s_memory->page_cursors[*page], &has_more,
                                         next, sizeof(next));
        if (error != ESP_OK) {
            return error;
        }
        message_ref_t messages[MESSAGE_PAGE_SIZE];
        size_t count;
        error = collect_messages(messages, MESSAGE_PAGE_SIZE, &count);
        if (error != ESP_OK) {
            return error;
        }
        if (!s_memory->last_message_id[0]) {
            if (!has_more) {
                *checkpoint = count;
                return ESP_OK;
            }
        } else {
            for (size_t index = 0; index < count; index++) {
                char message_id[sizeof(s_memory->last_message_id)];
                if (mp_json_get_string(messages[index].data, messages[index].length,
                                       "id", message_id, sizeof(message_id)) &&
                    strcmp(message_id, s_memory->last_message_id) == 0) {
                    *checkpoint = index;
                    return ESP_OK;
                }
            }
        }
        if (!has_more) {
            return ESP_ERR_NOT_FOUND;
        }
        if (*page + 1 >= MESSAGE_PAGE_COUNT) {
            return ESP_ERR_NO_MEM;
        }
        strlcpy(s_memory->page_cursors[*page + 1], next, sizeof(s_memory->page_cursors[0]));
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t process_page(size_t limit)
{
    message_ref_t messages[MESSAGE_PAGE_SIZE];
    size_t count;
    esp_err_t error = collect_messages(messages, MESSAGE_PAGE_SIZE, &count);
    if (error != ESP_OK) {
        return error;
    }
    if (limit < count) {
        count = limit;
    }
    while (count > 0) {
        count--;
        error = process_message(messages[count].data, messages[count].length);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

static esp_err_t collect_messages(message_ref_t *messages, size_t size, size_t *count)
{
    const char *array;
    size_t array_length;
    if (!mp_json_get_slice(s_memory->poll_response, strlen(s_memory->poll_response), "messages",
                           &array, &array_length)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *count = 0;
    size_t offset = 0;
    while (*count < size &&
           mp_json_next(array, array_length, &offset, &messages[*count].data,
                        &messages[*count].length)) {
        (*count)++;
    }
    return ESP_OK;
}

static esp_err_t process_message(const char *json, size_t length)
{
    char direction[12];
    if (!mp_json_get_string(json, length, "direction", direction, sizeof(direction)) ||
        strcmp(direction, "incoming") != 0) {
        return ESP_OK;
    }
    char message_id[sizeof(s_memory->last_message_id)];
    if (!mp_json_get_string(json, length, "id", message_id, sizeof(message_id))) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    bool deleted = false;
    mp_json_get_bool(json, length, "isDeleted", &deleted);
    if (deleted) {
        return checkpoint_save(message_id);
    }
    char chat_id[MP_CHAT_ID_LEN];
    snprintf(chat_id, sizeof(chat_id), CHAT_PREFIX "%.*s",
             (int)(sizeof(chat_id) - sizeof(CHAT_PREFIX)), s_memory->conversation_id);
    s_memory->message[0] = 0;
    mp_json_get_string(json, length, "message", s_memory->message, sizeof(s_memory->message));
    uint8_t image_count;
    size_t refs_used;
    esp_err_t media_error = collect_image_refs(json, length, &image_count, &refs_used);
    if (media_error != ESP_OK) {
        esp_err_t error = mp_instagram_send(
            chat_id, "This Instagram media message has unsupported, invalid or too many image attachments.");
        if (error == ESP_OK) {
            error = mp_instagram_flush(portMAX_DELAY);
        }
        return error == ESP_OK ? checkpoint_save(message_id) : error;
    }
    if (!s_memory->message[0] && !image_count) {
        esp_err_t error = mp_instagram_send(
            chat_id, "MicroPaw accepts text and public HTTPS image attachments.");
        if (error == ESP_OK) {
            error = mp_instagram_flush(portMAX_DELAY);
        }
        return error == ESP_OK ? checkpoint_save(message_id) : error;
    }
    esp_err_t error = image_count ?
                      mp_agent_submit_image_urls_wait(chat_id, s_memory->message,
                                                      s_memory->media_refs, refs_used,
                                                      image_count) :
                      mp_agent_submit_wait(chat_id, s_memory->message, false);
    if (error == ESP_OK) {
        error = mp_instagram_flush(portMAX_DELAY);
    }
    return error == ESP_OK ? checkpoint_save(message_id) : error;
}

static esp_err_t collect_image_refs(const char *json, size_t length, uint8_t *count,
                                    size_t *used)
{
    const char *attachments;
    size_t attachments_length;
    *count = 0;
    *used = 0;
    if (!mp_json_get_slice(json, length, "attachments",
                           &attachments, &attachments_length)) {
        return ESP_OK;
    }
    size_t offset = 0;
    const char *attachment;
    size_t attachment_length;
    while (mp_json_next(attachments, attachments_length, &offset,
                        &attachment, &attachment_length)) {
        char type[16];
        if (*count == UINT8_MAX ||
            !mp_json_get_string(attachment, attachment_length, "type",
                                type, sizeof(type)) ||
            strcmp(type, "image") != 0 ||
            *used >= sizeof(s_memory->media_refs) ||
            !mp_json_get_string(attachment, attachment_length, "url",
                                s_memory->media_refs + *used,
                                sizeof(s_memory->media_refs) - *used) ||
            !mp_url_is_public_https(s_memory->media_refs + *used) ||
            strpbrk(s_memory->media_refs + *used, " \t\r\n\"\\") != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        size_t url_length = strlen(s_memory->media_refs + *used);
        if (!url_length || url_length + 1 > sizeof(s_memory->media_refs) - *used) {
            return ESP_ERR_INVALID_SIZE;
        }
        *used += url_length + 1;
        (*count)++;
    }
    return ESP_OK;
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

static esp_err_t send_chunk(const char *chat_id, const char *text, size_t length)
{
    s_send_status = 0;
    char authorization[112];
    snprintf(authorization, sizeof(authorization), "Bearer %s",
             mp_config_get()->zernio_api_key);
    char part[OUTBOUND_TEXT_LEN];
    memcpy(part, text, length);
    part[length] = 0;
    mp_writer_t writer;
    mp_writer_init(&writer, s_memory->sender_body, sizeof(s_memory->sender_body));
    mp_writer_raw(&writer, "{\"accountId\":");
    mp_writer_string(&writer, s_memory->account_id);
    mp_writer_raw(&writer, ",\"message\":");
    mp_writer_string(&writer, part);
    mp_writer_char(&writer, '}');
    if (!writer.valid) {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[192];
    snprintf(url, sizeof(url),
             "https://zernio.com/api/v1/inbox/conversations/%.*s/messages",
             (int)(MP_CHAT_ID_LEN - sizeof(CHAT_PREFIX)),
             chat_id + strlen(CHAT_PREFIX));
    mp_http_header_t headers[] = {
        {"Authorization", authorization},
        {"Content-Type", "application/json"}
    };
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .headers = headers,
        .header_count = 2,
        .body = s_memory->sender_body,
        .body_size = writer.length,
        .response_limit = 1024,
        .timeout_ms = 20000,
        .buffer_size = 1024,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response = {0};
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
    esp_err_t error = mp_http_session_stream(&s_session, &request, NULL, NULL, &response);
    xSemaphoreGive(s_http_mutex);
    s_send_status = response.status;
    return error == ESP_OK && response.status == 200 ? ESP_OK :
           error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
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
        mp_metrics_error("instagram_send", error);
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
    if (xQueueSend(s_outbound_queue, &s_memory->enqueue, timeout) != pdTRUE) {
        pending_complete();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t allocate_memory(void)
{
    if (s_memory) {
        return ESP_OK;
    }
    s_memory = heap_caps_calloc(1, sizeof(*s_memory),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_task_stack = heap_caps_malloc(CONFIG_MICROPAW_INSTAGRAM_STACK *
                                    sizeof(*s_task_stack),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_sender_task_stack = heap_caps_malloc(CONFIG_MICROPAW_INSTAGRAM_SENDER_STACK *
                                           sizeof(*s_sender_task_stack),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_memory && s_task_stack && s_sender_task_stack) {
        return ESP_OK;
    }
    heap_caps_free(s_sender_task_stack);
    heap_caps_free(s_task_stack);
    heap_caps_free(s_memory);
    s_sender_task_stack = NULL;
    s_task_stack = NULL;
    s_memory = NULL;
    return ESP_ERR_NO_MEM;
}

esp_err_t mp_instagram_start(void)
{
    const mp_config_t *config = mp_config_get();
    if (strcmp(config->instagram_enabled, "true") != 0 ||
        !config->zernio_api_key[0] || !config->instagram_owner_username[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = allocate_memory();
    if (error != ESP_OK) {
        return error;
    }
    if (!s_outbound_queue) {
        s_outbound_queue = xQueueCreateStatic(OUTBOUND_QUEUE_LENGTH, sizeof(outbound_item_t),
                                              s_memory->outbound_storage,
                                              &s_outbound_queue_buffer);
        s_outbound_event = xEventGroupCreateStatic(&s_outbound_event_buffer);
        s_enqueue_mutex = xSemaphoreCreateMutexStatic(&s_enqueue_mutex_buffer);
        s_http_mutex = xSemaphoreCreateMutexStatic(&s_http_mutex_buffer);
        if (!s_outbound_queue || !s_outbound_event || !s_enqueue_mutex || !s_http_mutex) {
            return ESP_ERR_NO_MEM;
        }
        xEventGroupSetBits(s_outbound_event, OUTBOUND_IDLE);
    }
    if (!s_sender_task) {
        s_sender_task = xTaskCreateStaticPinnedToCore(
            sender_task, "mp_ig_send", CONFIG_MICROPAW_INSTAGRAM_SENDER_STACK,
            NULL, 4, s_sender_task_stack, &s_sender_task_buffer, 0);
        if (!s_sender_task) {
            return ESP_ERR_NO_MEM;
        }
        mp_metrics_register("instagram_sender", s_sender_task);
    }
    if (!s_task) {
        s_task = xTaskCreateStaticPinnedToCore(
            instagram_task, "mp_instagram", CONFIG_MICROPAW_INSTAGRAM_STACK,
            NULL, 4, s_task_stack, &s_task_buffer, 0);
        if (!s_task) {
            return ESP_ERR_NO_MEM;
        }
        mp_metrics_register("instagram", s_task);
    }
    return ESP_OK;
}

esp_err_t mp_instagram_send(const char *chat_id, const char *text)
{
    if (!s_outbound_queue || !mp_instagram_chat(chat_id) || !text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_enqueue_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t error = ESP_OK;
    size_t remaining = strlen(text);
    while (remaining) {
        size_t length = utf8_chunk(text, remaining, OUTBOUND_TEXT_LEN - 1);
        memset(&s_memory->enqueue, 0, sizeof(s_memory->enqueue));
        s_memory->enqueue.text_length = length;
        s_memory->enqueue.queued_us = esp_timer_get_time();
        strlcpy(s_memory->enqueue.chat_id, chat_id, sizeof(s_memory->enqueue.chat_id));
        memcpy(s_memory->enqueue.text, text, length);
        s_memory->enqueue.text[length] = 0;
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

esp_err_t mp_instagram_flush(TickType_t timeout)
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

bool mp_instagram_chat(const char *chat_id)
{
    return chat_id && strncmp(chat_id, CHAT_PREFIX, strlen(CHAT_PREFIX)) == 0 &&
           chat_id[strlen(CHAT_PREFIX)];
}

TaskHandle_t mp_instagram_task_handle(void)
{
    return s_task;
}

TaskHandle_t mp_instagram_sender_task_handle(void)
{
    return s_sender_task;
}
