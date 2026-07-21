#include "mp_confirmation.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint32_t id;
    int64_t expires_us;
    char chat_id[MP_CHAT_ID_LEN];
    char tool[MP_TOOL_NAME_LEN];
    char arguments[MP_TOOL_ARGS_LEN];
    bool active;
} pending_confirmation_t;

static pending_confirmation_t s_pending;
static uint32_t s_next_id = 1000;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;

static void ensure_lock(void);
static bool matches(const char *chat_id, uint32_t id);
esp_err_t mp_confirmation_request(const char *chat_id, const char *tool, const char *arguments,
                                  uint32_t *id);
esp_err_t mp_confirmation_take(const char *chat_id, uint32_t id, char *tool, size_t tool_size,
                               char *arguments, size_t arguments_size);
esp_err_t mp_confirmation_cancel(const char *chat_id, uint32_t id);

static void ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    }
}

static bool matches(const char *chat_id, uint32_t id)
{
    if (!s_pending.active || s_pending.id != id || strcmp(s_pending.chat_id, chat_id) != 0) {
        return false;
    }
    if (esp_timer_get_time() > s_pending.expires_us) {
        s_pending.active = false;
        return false;
    }
    return true;
}

esp_err_t mp_confirmation_request(const char *chat_id, const char *tool, const char *arguments,
                                  uint32_t *id)
{
    if (!chat_id || !tool || !arguments || strlen(chat_id) >= sizeof(s_pending.chat_id) ||
        strlen(tool) >= sizeof(s_pending.tool) || strlen(arguments) >= sizeof(s_pending.arguments)) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.id = s_next_id++;
    s_pending.expires_us = esp_timer_get_time() + 300000000LL;
    s_pending.active = true;
    strlcpy(s_pending.chat_id, chat_id, sizeof(s_pending.chat_id));
    strlcpy(s_pending.tool, tool, sizeof(s_pending.tool));
    strlcpy(s_pending.arguments, arguments, sizeof(s_pending.arguments));
    if (id) {
        *id = s_pending.id;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t mp_confirmation_take(const char *chat_id, uint32_t id, char *tool, size_t tool_size,
                               char *arguments, size_t arguments_size)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!matches(chat_id, id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(tool, s_pending.tool, tool_size);
    strlcpy(arguments, s_pending.arguments, arguments_size);
    s_pending.active = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t mp_confirmation_cancel(const char *chat_id, uint32_t id)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!matches(chat_id, id)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    s_pending.active = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
