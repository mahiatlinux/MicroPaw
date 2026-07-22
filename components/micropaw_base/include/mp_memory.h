#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "mp_types.h"

esp_err_t mp_memory_init(void);
esp_err_t mp_memory_reset(void);
esp_err_t mp_memory_save(const char *text);
void mp_memory_format(char *output, size_t size);
esp_err_t mp_history_add_exchange(const char *chat_id, const char *user,
                                  const char *assistant, const char *assistant_id);
size_t mp_history_get(const char *chat_id, char roles[][10],
                      char texts[][MP_HISTORY_TEXT_LEN], char ids[][MP_HISTORY_ID_LEN],
                      size_t count);
esp_err_t mp_history_clear(const char *chat_id);
