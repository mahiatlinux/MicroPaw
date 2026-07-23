#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "mp_types.h"

esp_err_t mp_memory_init(void);
esp_err_t mp_memory_reset(void);
esp_err_t mp_memory_save(const char *text);
void mp_memory_format(char *output, size_t size);
size_t mp_history_export(char *output, size_t size);
esp_err_t mp_history_erase(void);
