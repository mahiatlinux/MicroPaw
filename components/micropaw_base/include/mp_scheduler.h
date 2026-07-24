#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mp_types.h"

typedef struct {
    uint32_t id;
    int64_t next_epoch;
    uint32_t repeat_seconds;
    bool running;
} mp_schedule_info_t;

esp_err_t mp_scheduler_init(mp_schedule_emit_fn emit);
esp_err_t mp_scheduler_start(void);
esp_err_t mp_scheduler_reset(void);
esp_err_t mp_scheduler_add(const char *chat_id, const char *text, uint32_t delay_seconds,
                           uint32_t repeat_seconds, uint32_t *id, int64_t *next_epoch);
esp_err_t mp_scheduler_update(uint32_t id, const char *text, uint32_t delay_seconds,
                              uint32_t repeat_seconds, int64_t *next_epoch);
esp_err_t mp_scheduler_snooze(uint32_t id, uint32_t delay_seconds, int64_t *next_epoch);
esp_err_t mp_scheduler_run(uint32_t id);
esp_err_t mp_scheduler_delete(uint32_t id);
esp_err_t mp_scheduler_complete(uint32_t id, bool success);
bool mp_scheduler_should_run(uint32_t id);
esp_err_t mp_scheduler_missed_clear(void);
size_t mp_scheduler_count(void);
bool mp_scheduler_get(size_t index, mp_schedule_info_t *info, char *text, size_t text_size);
void mp_scheduler_format(char *output, size_t size);
void mp_scheduler_missed_format(char *output, size_t size);
