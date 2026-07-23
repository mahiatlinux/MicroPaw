#include "mp_scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp_metrics.h"
#include "nvs.h"
#include "sdkconfig.h"

#define SCHEDULE_MAGIC 0x4d505332U
#define SCHEDULE_RETRY_SECONDS 30

typedef struct {
    uint32_t id;
    int64_t next_epoch;
    uint32_t repeat_seconds;
    char chat_id[MP_CHAT_ID_LEN];
    char text[MP_SCHEDULE_TEXT_LEN];
    bool active;
} schedule_record_t;

typedef struct {
    uint32_t magic;
    uint32_t next_id;
    schedule_record_t records[MP_SCHEDULE_SLOTS];
} schedule_store_t;

EXT_RAM_BSS_ATTR static schedule_store_t s_store;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[CONFIG_MICROPAW_SCHEDULER_STACK];
static TaskHandle_t s_task;
static mp_schedule_emit_fn s_emit;
static bool s_inflight[MP_SCHEDULE_SLOTS];
static int64_t s_retry_epoch[MP_SCHEDULE_SLOTS];

static esp_err_t load_store(void);
static esp_err_t save_store(void);
static int free_slot(void);
static int find_slot(uint32_t id);
static void scheduler_task(void *argument);
static void scheduler_check(void);
esp_err_t mp_scheduler_init(mp_schedule_emit_fn emit);
esp_err_t mp_scheduler_start(void);
esp_err_t mp_scheduler_reset(void);
esp_err_t mp_scheduler_add(const char *chat_id, const char *text, uint32_t delay_seconds,
                           uint32_t repeat_seconds, uint32_t *id, int64_t *next_epoch);
esp_err_t mp_scheduler_delete(uint32_t id);
esp_err_t mp_scheduler_complete(uint32_t id, bool success);
void mp_scheduler_format(char *output, size_t size);

static esp_err_t load_store(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(s_store);
    esp_err_t error = nvs_open("mp_schedule", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_get_blob(handle, "jobs", &s_store, &size);
    nvs_close(handle);
    return error;
}

static esp_err_t save_store(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mp_schedule", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, "jobs", &s_store, sizeof(s_store));
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static int free_slot(void)
{
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        if (!s_store.records[index].active) {
            return index;
        }
    }
    return -1;
}

static int find_slot(uint32_t id)
{
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        if (s_store.records[index].active && s_store.records[index].id == id) {
            return index;
        }
    }
    return -1;
}

static void scheduler_task(void *argument)
{
    while (true) {
        scheduler_check();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void scheduler_check(void)
{
    int64_t now = (int64_t)time(NULL);
    if (now < 1700000000) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        schedule_record_t *record = &s_store.records[index];
        if (!record->active || s_inflight[index] || record->next_epoch > now ||
            s_retry_epoch[index] > now) {
            continue;
        }
        esp_err_t error = s_emit(record->id, record->chat_id, record->text);
        if (error == ESP_OK) {
            s_inflight[index] = true;
        } else {
            s_retry_epoch[index] = now + SCHEDULE_RETRY_SECONDS;
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t mp_scheduler_init(mp_schedule_emit_fn emit)
{
    if (!emit) {
        return ESP_ERR_INVALID_ARG;
    }
    s_emit = emit;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_store, 0, sizeof(s_store));
    memset(s_inflight, 0, sizeof(s_inflight));
    memset(s_retry_epoch, 0, sizeof(s_retry_epoch));
    if (load_store() != ESP_OK || s_store.magic != SCHEDULE_MAGIC) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.magic = SCHEDULE_MAGIC;
        s_store.next_id = 1;
    }
    return ESP_OK;
}

esp_err_t mp_scheduler_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_task = xTaskCreateStaticPinnedToCore(scheduler_task, "mp_schedule",
                                           CONFIG_MICROPAW_SCHEDULER_STACK, NULL, 4,
                                           s_task_stack, &s_task_buffer, 0);
    if (s_task) {
        mp_metrics_register("scheduler", s_task);
    }
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mp_scheduler_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_store, 0, sizeof(s_store));
    memset(s_inflight, 0, sizeof(s_inflight));
    memset(s_retry_epoch, 0, sizeof(s_retry_epoch));
    s_store.magic = SCHEDULE_MAGIC;
    s_store.next_id = 1;
    esp_err_t error = save_store();
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_scheduler_add(const char *chat_id, const char *text, uint32_t delay_seconds,
                           uint32_t repeat_seconds, uint32_t *id, int64_t *next_epoch)
{
    if (!chat_id || !text || !text[0] || delay_seconds == 0 ||
        strlen(chat_id) >= MP_CHAT_ID_LEN || strlen(text) >= MP_SCHEDULE_TEXT_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t now = (int64_t)time(NULL);
    if (now < 1700000000) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = free_slot();
    if (slot < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    schedule_record_t *record = &s_store.records[slot];
    memset(record, 0, sizeof(*record));
    record->id = s_store.next_id++;
    record->next_epoch = now + delay_seconds;
    record->repeat_seconds = repeat_seconds;
    record->active = true;
    strlcpy(record->chat_id, chat_id, sizeof(record->chat_id));
    strlcpy(record->text, text, sizeof(record->text));
    esp_err_t error = save_store();
    if (error == ESP_OK && id) {
        *id = record->id;
    }
    if (error == ESP_OK && next_epoch) {
        *next_epoch = record->next_epoch;
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_scheduler_delete(uint32_t id)
{
    esp_err_t error = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = find_slot(id);
    if (index >= 0) {
        s_store.records[index].active = false;
        s_inflight[index] = false;
        s_retry_epoch[index] = 0;
        error = save_store();
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_scheduler_complete(uint32_t id, bool success)
{
    esp_err_t error = ESP_ERR_NOT_FOUND;
    int64_t now = (int64_t)time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = find_slot(id);
    if (index >= 0) {
        schedule_record_t *record = &s_store.records[index];
        s_inflight[index] = false;
        if (!success) {
            s_retry_epoch[index] = now + SCHEDULE_RETRY_SECONDS;
            error = ESP_OK;
        } else {
            s_retry_epoch[index] = 0;
            if (record->repeat_seconds) {
                do {
                    record->next_epoch += record->repeat_seconds;
                } while (record->next_epoch <= now);
            } else {
                record->active = false;
            }
            error = save_store();
        }
    }
    xSemaphoreGive(s_lock);
    return error;
}

void mp_scheduler_format(char *output, size_t size)
{
    if (size == 0) {
        return;
    }
    output[0] = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        schedule_record_t *record = &s_store.records[index];
        if (!record->active) {
            continue;
        }
        size_t used = strnlen(output, size);
        if (used < size) {
            struct tm local;
            char when[32];
            time_t epoch = (time_t)record->next_epoch;
            localtime_r(&epoch, &local);
            strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S%z", &local);
            snprintf(output + used, size - used,
                     "id=%lu at=%s repeat=%lu state=%s text=%s\n",
                     (unsigned long)record->id, when,
                     (unsigned long)record->repeat_seconds,
                     s_inflight[index] ? "running" : "pending", record->text);
        }
    }
    xSemaphoreGive(s_lock);
    if (!output[0]) {
        strlcpy(output, "No scheduled jobs.", size);
    }
}
