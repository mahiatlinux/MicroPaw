#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mp_types.h"

#define MP_CALL_ID_LEN 128
#define MP_ITEM_ID_LEN 128

typedef struct {
    char text[MP_REPLY_LEN];
    char item_id[MP_ITEM_ID_LEN];
    char call_id[MP_CALL_ID_LEN];
    char tool[MP_TOOL_NAME_LEN];
    char arguments[MP_TOOL_ARGS_LEN];
    char error[192];
    bool has_tool;
} mp_llm_result_t;

bool mp_llm_ready(void);
esp_err_t mp_llm_stream(const char *body, mp_llm_result_t *result);
