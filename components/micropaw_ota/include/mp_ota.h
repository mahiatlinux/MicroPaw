#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef void (*mp_ota_progress_fn)(const char *text, void *context);

esp_err_t mp_ota_update(mp_ota_progress_fn progress, void *context,
                        char *output, size_t size);
esp_err_t mp_ota_confirm_running(void);
esp_err_t mp_ota_rollback_pending(void);
