#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t mp_telegram_start(void);
esp_err_t mp_telegram_send(const char *chat_id, const char *text);
esp_err_t mp_telegram_typing_start(const char *chat_id, int64_t queued_us);
void mp_telegram_typing_stop(const char *chat_id);
esp_err_t mp_telegram_flush(TickType_t timeout);
TaskHandle_t mp_telegram_task_handle(void);
TaskHandle_t mp_telegram_sender_task_handle(void);
