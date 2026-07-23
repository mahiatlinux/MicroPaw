#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp_types.h"

esp_err_t mp_agent_init(mp_send_fn send, mp_flush_fn flush, mp_finish_fn finish);
esp_err_t mp_agent_start(void);
esp_err_t mp_agent_submit(const char *chat_id, const char *text, bool proactive, TickType_t timeout);
esp_err_t mp_agent_submit_scheduled(uint32_t id, const char *chat_id, const char *text);
void mp_agent_reset_confirmation(void);
mp_agent_state_t mp_agent_state(void);
TaskHandle_t mp_agent_task_handle(void);
