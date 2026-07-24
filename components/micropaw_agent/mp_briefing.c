#include "mp_briefing.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp_agent.h"
#include "mp_config.h"
#include "mp_metrics.h"
#include "nvs.h"
#include "sdkconfig.h"

#define BRIEFING_MAGIC 0x4d504231U
#define BRIEFING_CATCHUP_SECONDS 14400

typedef struct {
    uint32_t magic;
    int32_t pending_day;
    int32_t delivered_day;
    int32_t skipped_day;
} briefing_store_t;

static briefing_store_t s_store;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[CONFIG_MICROPAW_BRIEFING_STACK];
static TaskHandle_t s_task;

static esp_err_t load_store(void);
static esp_err_t save_store(void);
static int32_t local_day(const struct tm *local);
static time_t due_time(time_t now, const char *time_text);
static void briefing_task(void *argument);
static void briefing_check(void);
esp_err_t mp_briefing_init(void);
esp_err_t mp_briefing_start(void);
esp_err_t mp_briefing_set_enabled(bool enabled);
esp_err_t mp_briefing_set_time(const char *time_text);
void mp_briefing_format(char *output, size_t size);

static esp_err_t load_store(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(s_store);
    esp_err_t error = nvs_open("mp_briefing", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_get_blob(handle, "state", &s_store, &size);
    nvs_close(handle);
    return error;
}

static esp_err_t save_store(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mp_briefing", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, "state", &s_store, sizeof(s_store));
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static int32_t local_day(const struct tm *local)
{
    return (local->tm_year + 1900) * 10000 +
           (local->tm_mon + 1) * 100 + local->tm_mday;
}

static time_t due_time(time_t now, const char *time_text)
{
    struct tm due;
    localtime_r(&now, &due);
    due.tm_hour = (time_text[0] - '0') * 10 + time_text[1] - '0';
    due.tm_min = (time_text[3] - '0') * 10 + time_text[4] - '0';
    due.tm_sec = 0;
    due.tm_isdst = -1;
    return mktime(&due);
}

static void briefing_task(void *argument)
{
    (void)argument;
    while (true) {
        briefing_check();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    }
}

static void briefing_check(void)
{
    const mp_config_t *config = mp_config_get();
    if (strcmp(config->morning_briefing_enabled, "true") != 0 ||
        !config->telegram_token[0] || !config->owner_chat_id[0]) {
        return;
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        return;
    }
    struct tm local;
    localtime_r(&now, &local);
    int32_t day = local_day(&local);
    time_t due = due_time(now, config->morning_briefing_time);
    if (now < due) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_store.delivered_day == day || s_store.skipped_day == day) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (now - due > BRIEFING_CATCHUP_SECONDS) {
        s_store.pending_day = 0;
        s_store.skipped_day = day;
        esp_err_t error = save_store();
        xSemaphoreGive(s_lock);
        if (error != ESP_OK) {
            mp_metrics_error("briefing_state", error);
        }
        return;
    }
    if (s_store.pending_day != day) {
        s_store.pending_day = day;
        esp_err_t error = save_store();
        if (error != ESP_OK) {
            xSemaphoreGive(s_lock);
            mp_metrics_error("briefing_state", error);
            return;
        }
    }
    xSemaphoreGive(s_lock);
    static const char prompt[] =
        "Prepare today's morning briefing for the owner. Call time_now first. "
#if CONFIG_MICROPAW_CALENDAR
        "If calendar access is enabled, list today's local calendar events. "
#endif
#if CONFIG_MICROPAW_GMAIL
        "If Gmail access is enabled, search useful unread mail and omit promotions and social mail. "
#endif
        "List upcoming reminders with schedule_list. Keep the briefing short and useful. "
        "Skip any disabled service and do not change mail, calendar or reminders.";
    int64_t started = esp_timer_get_time();
    esp_err_t error = mp_agent_submit_wait(config->owner_chat_id, prompt, true);
    mp_metrics_briefing((uint32_t)((esp_timer_get_time() - started) / 1000),
                        error == ESP_OK);
    if (error != ESP_OK) {
        mp_metrics_error("briefing", error);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_store.pending_day = 0;
    s_store.delivered_day = day;
    error = save_store();
    xSemaphoreGive(s_lock);
    if (error != ESP_OK) {
        mp_metrics_error("briefing_state", error);
    }
}

esp_err_t mp_briefing_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_store, 0, sizeof(s_store));
    if (load_store() != ESP_OK || s_store.magic != BRIEFING_MAGIC) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.magic = BRIEFING_MAGIC;
    }
    return ESP_OK;
}

esp_err_t mp_briefing_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_task = xTaskCreateStaticPinnedToCore(briefing_task, "mp_briefing",
                                           CONFIG_MICROPAW_BRIEFING_STACK, NULL, 4,
                                           s_task_stack, &s_task_buffer, 0);
    if (s_task) {
        mp_metrics_register("briefing", s_task);
    }
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mp_briefing_set_enabled(bool enabled)
{
    esp_err_t error = mp_config_set("morning_briefing_enabled",
                                    enabled ? "true" : "false");
    if (error == ESP_OK && s_task) {
        xTaskNotifyGive(s_task);
    }
    return error;
}

esp_err_t mp_briefing_set_time(const char *time_text)
{
    esp_err_t error = mp_config_set("morning_briefing_time", time_text);
    if (error == ESP_OK && s_task) {
        xTaskNotifyGive(s_task);
    }
    return error;
}

void mp_briefing_format(char *output, size_t size)
{
    if (!size) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(output, size, "Morning briefing is %s at %s. pending_day=%ld delivered_day=%ld skipped_day=%ld",
             strcmp(mp_config_get()->morning_briefing_enabled, "true") == 0 ? "on" : "off",
             mp_config_get()->morning_briefing_time, (long)s_store.pending_day,
             (long)s_store.delivered_day, (long)s_store.skipped_day);
    xSemaphoreGive(s_lock);
}
