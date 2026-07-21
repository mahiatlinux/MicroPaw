#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "mp_types.h"

esp_err_t mp_memory_init(void);
esp_err_t mp_memory_save(const char *text);
void mp_memory_format(char *output, size_t size);
esp_err_t mp_history_add(const char *role, const char *text);
size_t mp_history_get(char roles[][10], char texts[][MP_HISTORY_TEXT_LEN], size_t count);
esp_err_t mp_history_clear(void);
