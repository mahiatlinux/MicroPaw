#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mp_json.h"

#define MP_CONTEXT_SUMMARY_TEXT_LEN 32768
#define MP_CONTEXT_TURN_TRIGGER 6
#define MP_CONTEXT_BYTE_TRIGGER 131072
#define MP_CONTEXT_BYTE_TARGET 65536

typedef struct {
    uint32_t remove_count;
    size_t source_bytes;
} mp_context_compaction_t;

esp_err_t mp_context_init(void);
esp_err_t mp_context_append(const char *items, size_t size);
bool mp_context_format(mp_writer_t *writer);
bool mp_context_compaction_plan(mp_context_compaction_t *plan);
bool mp_context_format_compaction(mp_writer_t *writer, uint32_t remove_count);
esp_err_t mp_context_commit_summary(const char *summary_item, size_t size,
                                    uint32_t remove_count, uint32_t elapsed_ms);
esp_err_t mp_context_compaction_failed(uint32_t elapsed_ms);
esp_err_t mp_context_forget(void);
size_t mp_context_stored_bytes(void);
uint32_t mp_context_turn_count(void);
uint32_t mp_context_compaction_count(void);
uint32_t mp_context_compaction_failures(void);
uint32_t mp_context_last_compaction_ms(void);
