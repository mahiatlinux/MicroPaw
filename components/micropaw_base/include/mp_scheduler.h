#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mp_types.h"

esp_err_t mp_scheduler_init(mp_schedule_emit_fn emit);
esp_err_t mp_scheduler_start(void);
esp_err_t mp_scheduler_reset(void);
esp_err_t mp_scheduler_add(const char *chat_id, const char *text, uint32_t delay_seconds,
                           uint32_t repeat_seconds, uint32_t *id);
esp_err_t mp_scheduler_delete(uint32_t id);
void mp_scheduler_format(char *output, size_t size);
