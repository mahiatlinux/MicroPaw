#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t mp_instagram_start(void);
esp_err_t mp_instagram_send(const char *chat_id, const char *text);
esp_err_t mp_instagram_flush(TickType_t timeout);
bool mp_instagram_chat(const char *chat_id);
TaskHandle_t mp_instagram_task_handle(void);
TaskHandle_t mp_instagram_sender_task_handle(void);
