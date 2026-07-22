#include "mp_memory.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define MEMORY_MAGIC 0x4d504d32U
#define HISTORY_MAGIC 0x4d504832U

typedef struct {
    uint32_t sequence;
    char text[MP_MEMORY_TEXT_LEN];
} memory_record_t;

typedef struct {
    uint32_t magic;
    uint32_t next_sequence;
    memory_record_t records[MP_MEMORY_SLOTS];
} memory_store_t;

typedef struct {
    char chat_id[MP_CHAT_ID_LEN];
    char role[10];
    char text[MP_HISTORY_TEXT_LEN];
    char id[MP_HISTORY_ID_LEN];
} history_record_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    history_record_t records[MP_HISTORY_SLOTS];
} history_store_t;

EXT_RAM_BSS_ATTR static memory_store_t s_memory;
EXT_RAM_BSS_ATTR static history_store_t s_history;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;

static esp_err_t load_blob(const char *key, void *data, size_t size);
static esp_err_t save_blob(const char *key, const void *data, size_t size);
static int oldest_memory_slot(void);
static int next_memory_slot(uint32_t sequence);
esp_err_t mp_memory_init(void);
esp_err_t mp_memory_reset(void);
esp_err_t mp_memory_save(const char *text);
void mp_memory_format(char *output, size_t size);
esp_err_t mp_history_add_exchange(const char *chat_id, const char *user,
                                  const char *assistant, const char *assistant_id);
size_t mp_history_get(const char *chat_id, char roles[][10],
                      char texts[][MP_HISTORY_TEXT_LEN], char ids[][MP_HISTORY_ID_LEN],
                      size_t count);
esp_err_t mp_history_clear(const char *chat_id);

static esp_err_t load_blob(const char *key, void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mp_memory", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return error;
    }
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_get_blob(handle, key, data, &size);
    nvs_close(handle);
    return error;
}

static esp_err_t save_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mp_memory", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, key, data, size);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static int oldest_memory_slot(void)
{
    int slot = 0;
    uint32_t sequence = UINT32_MAX;
    for (int index = 0; index < MP_MEMORY_SLOTS; index++) {
        if (s_memory.records[index].sequence == 0) {
            return index;
        }
        if (s_memory.records[index].sequence < sequence) {
            sequence = s_memory.records[index].sequence;
            slot = index;
        }
    }
    return slot;
}

static int next_memory_slot(uint32_t sequence)
{
    int slot = -1;
    uint32_t next = UINT32_MAX;
    for (int index = 0; index < MP_MEMORY_SLOTS; index++) {
        uint32_t candidate = s_memory.records[index].sequence;
        if (candidate > sequence && candidate < next) {
            next = candidate;
            slot = index;
        }
    }
    return slot;
}

esp_err_t mp_memory_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_memory, 0, sizeof(s_memory));
    memset(&s_history, 0, sizeof(s_history));
    if (load_blob("facts", &s_memory, sizeof(s_memory)) != ESP_OK || s_memory.magic != MEMORY_MAGIC) {
        memset(&s_memory, 0, sizeof(s_memory));
        s_memory.magic = MEMORY_MAGIC;
        s_memory.next_sequence = 1;
    }
    if (load_blob("history", &s_history, sizeof(s_history)) != ESP_OK || s_history.magic != HISTORY_MAGIC ||
        s_history.count > MP_HISTORY_SLOTS) {
        memset(&s_history, 0, sizeof(s_history));
        s_history.magic = HISTORY_MAGIC;
    }
    return ESP_OK;
}

esp_err_t mp_memory_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_memory, 0, sizeof(s_memory));
    memset(&s_history, 0, sizeof(s_history));
    s_memory.magic = MEMORY_MAGIC;
    s_memory.next_sequence = 1;
    s_history.magic = HISTORY_MAGIC;
    esp_err_t error = save_blob("facts", &s_memory, sizeof(s_memory));
    if (error == ESP_OK) {
        error = save_blob("history", &s_history, sizeof(s_history));
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_memory_save(const char *text)
{
    if (!text || !text[0] || strlen(text) >= MP_MEMORY_TEXT_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = oldest_memory_slot();
    s_memory.records[slot].sequence = s_memory.next_sequence++;
    strlcpy(s_memory.records[slot].text, text, sizeof(s_memory.records[slot].text));
    esp_err_t error = save_blob("facts", &s_memory, sizeof(s_memory));
    xSemaphoreGive(s_lock);
    return error;
}

void mp_memory_format(char *output, size_t size)
{
    if (size == 0) {
        return;
    }
    output[0] = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t sequence = 0;
    for (int count = 0; count < MP_MEMORY_SLOTS; count++) {
        int index = next_memory_slot(sequence);
        if (index < 0) {
            break;
        }
        size_t used = strnlen(output, size);
        if (used < size) {
            snprintf(output + used, size - used, "- %s\n", s_memory.records[index].text);
        }
        sequence = s_memory.records[index].sequence;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t mp_history_add_exchange(const char *chat_id, const char *user,
                                  const char *assistant, const char *assistant_id)
{
    if (!chat_id || !user || !assistant || !assistant_id || !chat_id[0] || !user[0] ||
        !assistant[0] || !assistant_id[0] || strlen(chat_id) >= MP_CHAT_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_history.count > MP_HISTORY_SLOTS - 2) {
        memmove(&s_history.records[0], &s_history.records[2],
                sizeof(s_history.records[0]) * (MP_HISTORY_SLOTS - 2));
        s_history.count -= 2;
    }
    history_record_t *user_record = &s_history.records[s_history.count++];
    history_record_t *assistant_record = &s_history.records[s_history.count++];
    memset(user_record, 0, sizeof(*user_record));
    memset(assistant_record, 0, sizeof(*assistant_record));
    strlcpy(user_record->chat_id, chat_id, sizeof(user_record->chat_id));
    strlcpy(user_record->role, "user", sizeof(user_record->role));
    strlcpy(user_record->text, user, sizeof(user_record->text));
    strlcpy(assistant_record->chat_id, chat_id, sizeof(assistant_record->chat_id));
    strlcpy(assistant_record->role, "assistant", sizeof(assistant_record->role));
    strlcpy(assistant_record->text, assistant, sizeof(assistant_record->text));
    strlcpy(assistant_record->id, assistant_id, sizeof(assistant_record->id));
    esp_err_t error = save_blob("history", &s_history, sizeof(s_history));
    xSemaphoreGive(s_lock);
    return error;
}

size_t mp_history_get(const char *chat_id, char roles[][10],
                      char texts[][MP_HISTORY_TEXT_LEN], char ids[][MP_HISTORY_ID_LEN],
                      size_t count)
{
    if (!chat_id || !roles || !texts || !ids || !count) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t matched = 0;
    for (size_t index = 0; index < s_history.count; index++) {
        if (strcmp(s_history.records[index].chat_id, chat_id) == 0) {
            matched++;
        }
    }
    size_t skip = matched > count ? matched - count : 0;
    size_t copied = 0;
    for (size_t index = 0; index < s_history.count && copied < count; index++) {
        history_record_t *record = &s_history.records[index];
        if (strcmp(record->chat_id, chat_id) != 0) {
            continue;
        }
        if (skip) {
            skip--;
            continue;
        }
        strlcpy(roles[copied], record->role, 10);
        strlcpy(texts[copied], record->text, MP_HISTORY_TEXT_LEN);
        strlcpy(ids[copied], record->id, MP_HISTORY_ID_LEN);
        copied++;
    }
    xSemaphoreGive(s_lock);
    return copied;
}

esp_err_t mp_history_clear(const char *chat_id)
{
    if (!chat_id || !chat_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t kept = 0;
    for (size_t index = 0; index < s_history.count; index++) {
        if (strcmp(s_history.records[index].chat_id, chat_id) != 0) {
            s_history.records[kept++] = s_history.records[index];
        }
    }
    memset(&s_history.records[kept], 0,
           sizeof(s_history.records[0]) * (MP_HISTORY_SLOTS - kept));
    s_history.count = kept;
    esp_err_t error = save_blob("history", &s_history, sizeof(s_history));
    xSemaphoreGive(s_lock);
    return error;
}
