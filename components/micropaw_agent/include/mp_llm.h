#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mp_types.h"

#define MP_CALL_ID_LEN 128
#define MP_ITEM_ID_LEN 128
#define MP_LLM_CALL_ARENA_LEN (MP_TOOL_ARGS_LEN + 65536)

typedef enum {
    MP_LLM_CALL_PENDING,
    MP_LLM_CALL_READY,
    MP_LLM_CALL_DISCARDED
} mp_llm_call_state_t;

typedef struct {
    uint32_t size;
    uint32_t arguments_length;
    uint32_t order;
    mp_llm_call_state_t state;
    char item_id[MP_ITEM_ID_LEN];
    char call_id[MP_CALL_ID_LEN];
    char name[MP_TOOL_NAME_LEN];
    char arguments[];
} mp_llm_call_t;

typedef struct {
    char text[MP_REPLY_LEN];
    char message_id[MP_ITEM_ID_LEN];
    char error[192];
    size_t text_length;
    size_t arena_used;
    size_t call_count;
    uint32_t next_call_order;
    uint32_t arena[(MP_LLM_CALL_ARENA_LEN + 3) / 4];
} mp_llm_result_t;

esp_err_t mp_llm_init(void);
bool mp_llm_ready(void);
const char *mp_llm_boot_id(void);
void mp_llm_parse_begin(mp_llm_result_t *result);
bool mp_llm_parse_chunk(mp_llm_result_t *result, const uint8_t *data, size_t size);
esp_err_t mp_llm_parse_finish(mp_llm_result_t *result);
const mp_llm_call_t *mp_llm_call_next(const mp_llm_result_t *result, size_t *offset);
esp_err_t mp_llm_stream(const char *body, mp_llm_result_t *result);
esp_err_t mp_llm_stream_image(const char *body, size_t image_offset,
                              const uint8_t *data, size_t size,
                              mp_llm_result_t *result);
esp_err_t mp_llm_transcribe(const uint8_t *data, size_t size, const char *mime_type,
                            char *output, size_t output_size);
