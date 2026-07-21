#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t mp_telegram_start(void);
esp_err_t mp_telegram_send(const char *chat_id, const char *text);
TaskHandle_t mp_telegram_task_handle(void);
