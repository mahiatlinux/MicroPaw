#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t mp_time_parse_local(const char *date, const char *clock,
                              const char *meridiem, int64_t *epoch);
int64_t mp_time_repeat_next(int64_t epoch, uint32_t repeat_seconds);
