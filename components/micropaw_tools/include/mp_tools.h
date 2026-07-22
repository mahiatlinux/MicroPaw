#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "mp_types.h"

esp_err_t mp_tools_init(void);
bool mp_tools_json(char *output, size_t size);
esp_err_t mp_tools_execute(const char *name, const char *arguments,
                           const mp_tool_context_t *context, bool confirmed,
                           char *output, size_t size);
esp_err_t mp_tools_execute_scheduled(const char *text, const mp_tool_context_t *context,
                                     char *output, size_t size);
esp_err_t mp_page_fetch(const char *url, char *output, size_t size);
