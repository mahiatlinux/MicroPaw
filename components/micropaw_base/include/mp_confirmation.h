#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mp_types.h"

esp_err_t mp_confirmation_request(const char *chat_id, const char *tool, const char *arguments,
                                  uint32_t *id);
esp_err_t mp_confirmation_take(const char *chat_id, uint32_t id, char *tool, size_t tool_size,
                               char *arguments, size_t arguments_size);
esp_err_t mp_confirmation_cancel(const char *chat_id, uint32_t id);
