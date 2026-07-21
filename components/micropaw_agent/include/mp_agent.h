#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp_types.h"

esp_err_t mp_agent_init(mp_send_fn send);
esp_err_t mp_agent_start(void);
esp_err_t mp_agent_submit(const char *chat_id, const char *text, bool proactive, TickType_t timeout);
mp_agent_state_t mp_agent_state(void);
TaskHandle_t mp_agent_task_handle(void);
