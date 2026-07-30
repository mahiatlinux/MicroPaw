#include "mp_scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp_metrics.h"
#include "mp_time.h"
#include "nvs.h"
#include "sdkconfig.h"

#define SCHEDULE_MAGIC 0x4d505332U
#define MISSED_MAGIC 0x4d504d31U
#define SCHEDULE_RETRY_SECONDS 30
#define SCHEDULE_MISSED_SECONDS 60

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

typedef struct {
    uint32_t job_id;
    uint32_t sequence;
    uint32_t count;
    int64_t due_epoch;
    char chat_id[MP_CHAT_ID_LEN];
    char text[MP_SCHEDULE_TEXT_LEN];
    bool active;
    bool delivered;
} missed_record_t;

typedef struct {
    uint32_t magic;
    uint32_t next_sequence;
    missed_record_t records[MP_SCHEDULE_SLOTS];
} missed_store_t;

EXT_RAM_BSS_ATTR static schedule_store_t s_store;
EXT_RAM_BSS_ATTR static missed_store_t s_missed;
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
static esp_err_t load_missed(void);
static esp_err_t save_missed(void);
static esp_err_t save_both(void);
static int free_slot(void);
static int find_slot(uint32_t id);
static int find_pending_missed(uint32_t id);
static int missed_slot(void);
static bool cancel_pending_missed(uint32_t id);
static esp_err_t mark_missed(int index, int64_t now, int *missed_index);
static void format_epoch(int64_t epoch, char *output, size_t size);
static void scheduler_task(void *argument);
static void scheduler_check(void);
esp_err_t mp_scheduler_init(mp_schedule_emit_fn emit);
esp_err_t mp_scheduler_start(void);
esp_err_t mp_scheduler_reset(void);
esp_err_t mp_scheduler_add(const char *chat_id, const char *text, int64_t next_epoch,
                           uint32_t repeat_seconds, uint32_t *id);
esp_err_t mp_scheduler_update(uint32_t id, const char *text, int64_t next_epoch,
                              uint32_t repeat_seconds);
esp_err_t mp_scheduler_snooze(uint32_t id, int64_t next_epoch);
esp_err_t mp_scheduler_run(uint32_t id);
esp_err_t mp_scheduler_delete(uint32_t id);
esp_err_t mp_scheduler_complete(uint32_t id, bool success);
bool mp_scheduler_should_run(uint32_t id);
esp_err_t mp_scheduler_missed_clear(void);
size_t mp_scheduler_count(void);
bool mp_scheduler_get(size_t index, mp_schedule_info_t *info, char *text, size_t text_size);
bool mp_scheduler_find(uint32_t id, mp_schedule_info_t *info, char *text, size_t text_size);
void mp_scheduler_format(char *output, size_t size);
void mp_scheduler_missed_format(char *output, size_t size);

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

static esp_err_t load_missed(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(s_missed);
    esp_err_t error = nvs_open("mp_schedule", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_get_blob(handle, "missed", &s_missed, &size);
    nvs_close(handle);
    return error;
}

static esp_err_t save_missed(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mp_schedule", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, "missed", &s_missed, sizeof(s_missed));
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static esp_err_t save_both(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mp_schedule", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, "jobs", &s_store, sizeof(s_store));
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, "missed", &s_missed, sizeof(s_missed));
    }
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

static int find_pending_missed(uint32_t id)
{
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        missed_record_t *record = &s_missed.records[index];
        if (record->active && !record->delivered && record->job_id == id) {
            return index;
        }
    }
    return -1;
}

static int missed_slot(void)
{
    int oldest = -1;
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        missed_record_t *record = &s_missed.records[index];
        if (!record->active) {
            return index;
        }
        if (record->delivered &&
            (oldest < 0 || record->sequence < s_missed.records[oldest].sequence)) {
            oldest = index;
        }
    }
    return oldest;
}

static bool cancel_pending_missed(uint32_t id)
{
    int index = find_pending_missed(id);
    if (index < 0) {
        return false;
    }
    memset(&s_missed.records[index], 0, sizeof(s_missed.records[index]));
    return true;
}

static esp_err_t mark_missed(int index, int64_t now, int *missed_index)
{
    schedule_record_t *job = &s_store.records[index];
    uint64_t late_ms = (uint64_t)(now - job->next_epoch) * 1000;
    uint32_t metric_ms = late_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)late_ms;
    uint32_t count = 1;
    if (job->repeat_seconds) {
        count += (uint32_t)((now - job->next_epoch) / job->repeat_seconds);
    }
    int slot = find_pending_missed(job->id);
    if (slot >= 0) {
        if (count > s_missed.records[slot].count) {
            mp_metrics_missed(count - s_missed.records[slot].count, metric_ms);
            s_missed.records[slot].count = count;
            esp_err_t error = save_missed();
            if (error != ESP_OK) {
                return error;
            }
        }
        *missed_index = slot;
        return ESP_OK;
    }
    slot = missed_slot();
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    missed_record_t *record = &s_missed.records[slot];
    memset(record, 0, sizeof(*record));
    record->job_id = job->id;
    record->sequence = s_missed.next_sequence++;
    record->count = count;
    record->due_epoch = job->next_epoch;
    record->active = true;
    mp_metrics_missed(count, metric_ms);
    strlcpy(record->chat_id, job->chat_id, sizeof(record->chat_id));
    strlcpy(record->text, job->text, sizeof(record->text));
    esp_err_t error = save_missed();
    if (error == ESP_OK) {
        *missed_index = slot;
    }
    return error;
}

static void format_epoch(int64_t epoch, char *output, size_t size)
{
    time_t due = (time_t)epoch;
    struct tm local;
    localtime_r(&due, &local);
    strftime(output, size, "%Y-%m-%dT%H:%M:%S%z", &local);
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
        if (now - record->next_epoch >= SCHEDULE_MISSED_SECONDS) {
            int missed_index;
            esp_err_t error = mark_missed(index, now, &missed_index);
            if (error != ESP_OK) {
                s_retry_epoch[index] = now + SCHEDULE_RETRY_SECONDS;
                continue;
            }
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
    memset(&s_missed, 0, sizeof(s_missed));
    memset(s_inflight, 0, sizeof(s_inflight));
    memset(s_retry_epoch, 0, sizeof(s_retry_epoch));
    if (load_store() != ESP_OK || s_store.magic != SCHEDULE_MAGIC) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.magic = SCHEDULE_MAGIC;
        s_store.next_id = 1;
    }
    if (load_missed() != ESP_OK || s_missed.magic != MISSED_MAGIC) {
        memset(&s_missed, 0, sizeof(s_missed));
        s_missed.magic = MISSED_MAGIC;
        s_missed.next_sequence = 1;
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
    memset(&s_missed, 0, sizeof(s_missed));
    memset(s_inflight, 0, sizeof(s_inflight));
    memset(s_retry_epoch, 0, sizeof(s_retry_epoch));
    s_store.magic = SCHEDULE_MAGIC;
    s_store.next_id = 1;
    s_missed.magic = MISSED_MAGIC;
    s_missed.next_sequence = 1;
    esp_err_t error = save_both();
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_scheduler_add(const char *chat_id, const char *text, int64_t next_epoch,
                           uint32_t repeat_seconds, uint32_t *id)
{
    int64_t now = (int64_t)time(NULL);
    if (!chat_id || !text || !text[0] || now < 1700000000 || next_epoch <= now ||
        strlen(chat_id) >= MP_CHAT_ID_LEN || strlen(text) >= MP_SCHEDULE_TEXT_LEN) {
        return ESP_ERR_INVALID_ARG;
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
    record->next_epoch = next_epoch;
    record->repeat_seconds = repeat_seconds;
    record->active = true;
    strlcpy(record->chat_id, chat_id, sizeof(record->chat_id));
    strlcpy(record->text, text, sizeof(record->text));
    esp_err_t error = save_store();
    if (error == ESP_OK && id) {
        *id = record->id;
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_scheduler_update(uint32_t id, const char *text, int64_t next_epoch,
                              uint32_t repeat_seconds)
{
    int64_t now = (int64_t)time(NULL);
    if (!text || !text[0] || strlen(text) >= MP_SCHEDULE_TEXT_LEN ||
        now < 1700000000 || next_epoch <= now) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = find_slot(id);
    esp_err_t error = index < 0 ? ESP_ERR_NOT_FOUND :
                      s_inflight[index] ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (error == ESP_OK) {
        schedule_record_t *record = &s_store.records[index];
        record->next_epoch = next_epoch;
        record->repeat_seconds = repeat_seconds;
        strlcpy(record->text, text, sizeof(record->text));
        s_retry_epoch[index] = 0;
        bool missed_changed = cancel_pending_missed(id);
        error = missed_changed ? save_both() : save_store();
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_scheduler_snooze(uint32_t id, int64_t next_epoch)
{
    int64_t now = (int64_t)time(NULL);
    if (now < 1700000000 || next_epoch <= now) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = find_slot(id);
    esp_err_t error = index < 0 ? ESP_ERR_NOT_FOUND :
                      s_inflight[index] ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (error == ESP_OK) {
        schedule_record_t *record = &s_store.records[index];
        record->next_epoch = next_epoch;
        s_retry_epoch[index] = 0;
        bool missed_changed = cancel_pending_missed(id);
        error = missed_changed ? save_both() : save_store();
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_scheduler_run(uint32_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = find_slot(id);
    esp_err_t error = index < 0 ? ESP_ERR_NOT_FOUND :
                      s_emit(0, s_store.records[index].chat_id,
                             s_store.records[index].text);
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
        bool missed_changed = cancel_pending_missed(id);
        error = missed_changed ? save_both() : save_store();
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
            int missed_index = find_pending_missed(id);
            if (missed_index >= 0) {
                if (record->repeat_seconds) {
                    uint32_t count = 1 +
                        (uint32_t)((now - record->next_epoch) / record->repeat_seconds);
                    if (count > s_missed.records[missed_index].count) {
                        uint64_t late_ms = (uint64_t)(now - record->next_epoch) * 1000;
                        mp_metrics_missed(count - s_missed.records[missed_index].count,
                                          late_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)late_ms);
                        s_missed.records[missed_index].count = count;
                    }
                }
                s_missed.records[missed_index].delivered = true;
            }
            if (record->repeat_seconds) {
                do {
                    record->next_epoch =
                        mp_time_repeat_next(record->next_epoch, record->repeat_seconds);
                } while (record->next_epoch <= now);
            } else {
                record->active = false;
            }
            error = missed_index >= 0 ? save_both() : save_store();
        }
    }
    xSemaphoreGive(s_lock);
    return error;
}

bool mp_scheduler_should_run(uint32_t id)
{
    bool run = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = find_slot(id);
    if (index >= 0) {
        run = s_inflight[index];
    }
    xSemaphoreGive(s_lock);
    return run;
}

esp_err_t mp_scheduler_missed_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        if (s_missed.records[index].active && s_missed.records[index].delivered) {
            memset(&s_missed.records[index], 0, sizeof(s_missed.records[index]));
        }
    }
    esp_err_t error = save_missed();
    xSemaphoreGive(s_lock);
    return error;
}

size_t mp_scheduler_count(void)
{
    size_t count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        if (s_store.records[index].active) {
            count++;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

bool mp_scheduler_get(size_t index, mp_schedule_info_t *info, char *text, size_t text_size)
{
    if (!info || !text || !text_size) {
        return false;
    }
    bool found = false;
    size_t current = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int slot = 0; slot < MP_SCHEDULE_SLOTS; slot++) {
        schedule_record_t *record = &s_store.records[slot];
        if (!record->active) {
            continue;
        }
        if (current++ == index) {
            info->id = record->id;
            info->next_epoch = record->next_epoch;
            info->repeat_seconds = record->repeat_seconds;
            info->running = s_inflight[slot];
            strlcpy(text, record->text, text_size);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool mp_scheduler_find(uint32_t id, mp_schedule_info_t *info, char *text, size_t text_size)
{
    if (!info || !text || !text_size) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int index = find_slot(id);
    if (index >= 0) {
        schedule_record_t *record = &s_store.records[index];
        info->id = record->id;
        info->next_epoch = record->next_epoch;
        info->repeat_seconds = record->repeat_seconds;
        info->running = s_inflight[index];
        strlcpy(text, record->text, text_size);
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
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
            char when[32];
            const char *kind = "reminder";
            const char *text = record->text;
            if (strncmp(text, "email_send:", 11) == 0) {
                kind = "email";
                text += 11;
            }
            format_epoch(record->next_epoch, when, sizeof(when));
            snprintf(output + used, size - used,
                     "id=%lu kind=%s at=%s repeat=%lu state=%s data=%s\n",
                     (unsigned long)record->id, kind, when,
                     (unsigned long)record->repeat_seconds,
                     s_inflight[index] ? "running" : "pending", text);
        }
    }
    xSemaphoreGive(s_lock);
    if (!output[0]) {
        strlcpy(output, "No scheduled jobs.", size);
    }
}

void mp_scheduler_missed_format(char *output, size_t size)
{
    if (!size) {
        return;
    }
    output[0] = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int index = 0; index < MP_SCHEDULE_SLOTS; index++) {
        missed_record_t *record = &s_missed.records[index];
        if (!record->active) {
            continue;
        }
        size_t used = strnlen(output, size);
        if (used < size) {
            char due[32];
            format_epoch(record->due_epoch, due, sizeof(due));
            snprintf(output + used, size - used,
                     "job=%lu due=%s count=%lu state=%s text=%s\n",
                     (unsigned long)record->job_id, due,
                     (unsigned long)record->count,
                     record->delivered ? "delivered" : "pending", record->text);
        }
    }
    xSemaphoreGive(s_lock);
    if (!output[0]) {
        strlcpy(output, "No missed reminders.", size);
    }
}
