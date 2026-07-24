#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MP_CHAT_ID_LEN 24
#define MP_MESSAGE_LEN 1024
#define MP_REPLY_LEN CONFIG_MICROPAW_WORK_TEXT_BYTES
#define MP_TOOL_NAME_LEN 40
#define MP_TOOL_ARGS_LEN (CONFIG_MICROPAW_WORK_TEXT_BYTES * 2 + 4096)
#define MP_TOOL_RESULT_LEN 65536
#define MP_AGENT_REQUEST_LEN (CONFIG_MICROPAW_WORK_TEXT_BYTES * 6 + 65536)
#define MP_TOOL_TRACE_LEN (CONFIG_MICROPAW_WORK_TEXT_BYTES * 4 + 32768)
#define MP_MEMORY_SLOTS 8
#define MP_MEMORY_TEXT_LEN 1024
#define MP_HISTORY_SLOTS 8
#define MP_HISTORY_TEXT_LEN 512
#define MP_HISTORY_ID_LEN 128
#define MP_SCHEDULE_SLOTS 8
#define MP_SCHEDULE_TEXT_LEN 1024
#define MP_MEDIA_REF_ARENA_LEN 1024

typedef enum {
    MP_MEDIA_NONE,
    MP_MEDIA_IMAGE_BYTES,
    MP_MEDIA_IMAGE_URLS
} mp_media_kind_t;

typedef enum {
    MP_AGENT_IDLE,
    MP_AGENT_CONTEXT,
    MP_AGENT_INFERENCE,
    MP_AGENT_TOOL,
    MP_AGENT_RESPONSE,
    MP_AGENT_ERROR
} mp_agent_state_t;

typedef struct {
    char chat_id[MP_CHAT_ID_LEN];
    char text[MP_MESSAGE_LEN];
    int64_t queued_us;
    uint32_t schedule_id;
    uintptr_t media_data;
    uint32_t media_size;
    uint16_t media_refs_size;
    uint8_t media_count;
    mp_media_kind_t media_kind;
    bool proactive;
    TaskHandle_t waiter;
    char media_refs[MP_MEDIA_REF_ARENA_LEN];
} mp_message_t;

typedef struct {
    char chat_id[MP_CHAT_ID_LEN];
} mp_tool_context_t;

typedef esp_err_t (*mp_send_fn)(const char *chat_id, const char *text);
typedef esp_err_t (*mp_flush_fn)(const char *chat_id, TickType_t timeout);
typedef void (*mp_finish_fn)(const char *chat_id);
typedef esp_err_t (*mp_schedule_emit_fn)(uint32_t id, const char *chat_id, const char *text);
