#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t mp_briefing_init(void);
esp_err_t mp_briefing_start(void);
esp_err_t mp_briefing_set_enabled(bool enabled);
esp_err_t mp_briefing_set_time(const char *time_text);
void mp_briefing_format(char *output, size_t size);
