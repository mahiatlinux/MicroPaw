#include "mp_confirmation.h"

#include <stdbool.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "mp_types.h"

#define CONFIRM_ARENA_LEN (MP_TOOL_ARGS_LEN + 65536)

typedef struct {
    uint32_t size;
    uint32_t id;
    int64_t expires_us;
    uint16_t chat_length;
    uint16_t tool_length;
    uint32_t arguments_length;
} pending_header_t;

EXT_RAM_BSS_ATTR static uint32_t s_arena[(CONFIRM_ARENA_LEN + 3) / 4];
static size_t s_used;
static uint32_t s_next_id = 1000;

static pending_header_t *record_at(size_t offset);
static char *record_chat(pending_header_t *record);
static char *record_tool(pending_header_t *record);
static char *record_arguments(pending_header_t *record);
static void remove_record(size_t offset);
static void purge_expired(void);
static bool find_record(const char *chat_id, uint32_t id, size_t *offset);
esp_err_t mp_confirmation_request(const char *chat_id, const char *tool, const char *arguments,
                                  uint32_t *id);
void mp_confirmation_reset(void);
esp_err_t mp_confirmation_take(const char *chat_id, uint32_t id, char *tool, size_t tool_size,
                               char *arguments, size_t arguments_size);
esp_err_t mp_confirmation_cancel(const char *chat_id, uint32_t id);

static pending_header_t *record_at(size_t offset)
{
    return (pending_header_t *)((uint8_t *)s_arena + offset);
}

static char *record_chat(pending_header_t *record)
{
    return (char *)(record + 1);
}

static char *record_tool(pending_header_t *record)
{
    return record_chat(record) + record->chat_length + 1;
}

static char *record_arguments(pending_header_t *record)
{
    return record_tool(record) + record->tool_length + 1;
}

static void remove_record(size_t offset)
{
    pending_header_t *record = record_at(offset);
    size_t record_size = record->size;
    size_t remaining = s_used - offset - record_size;
    memmove((uint8_t *)s_arena + offset, (uint8_t *)s_arena + offset + record_size, remaining);
    s_used -= record_size;
    memset((uint8_t *)s_arena + s_used, 0, record_size);
}

static void purge_expired(void)
{
    int64_t now = esp_timer_get_time();
    size_t offset = 0;
    while (offset < s_used) {
        pending_header_t *record = record_at(offset);
        if (now > record->expires_us) {
            remove_record(offset);
        } else {
            offset += record->size;
        }
    }
}

static bool find_record(const char *chat_id, uint32_t id, size_t *offset)
{
    purge_expired();
    for (*offset = 0; *offset < s_used; *offset += record_at(*offset)->size) {
        pending_header_t *record = record_at(*offset);
        if (record->id == id && strcmp(record_chat(record), chat_id) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t mp_confirmation_request(const char *chat_id, const char *tool, const char *arguments,
                                  uint32_t *id)
{
    if (!chat_id || !tool || !arguments || !id) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t chat_length = strlen(chat_id);
    size_t tool_length = strlen(tool);
    size_t arguments_length = strlen(arguments);
    if (!chat_length || chat_length >= MP_CHAT_ID_LEN || !tool_length ||
        tool_length >= MP_TOOL_NAME_LEN || arguments_length > MP_TOOL_ARGS_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    purge_expired();
    size_t record_size = sizeof(pending_header_t) + chat_length + tool_length +
                         arguments_length + 3;
    record_size = (record_size + 3) & ~3U;
    if (record_size > sizeof(s_arena) - s_used) {
        return ESP_ERR_NO_MEM;
    }
    pending_header_t *record = record_at(s_used);
    memset(record, 0, record_size);
    record->size = record_size;
    record->id = s_next_id++;
    record->expires_us = esp_timer_get_time() + 300000000LL;
    record->chat_length = chat_length;
    record->tool_length = tool_length;
    record->arguments_length = arguments_length;
    memcpy(record_chat(record), chat_id, chat_length + 1);
    memcpy(record_tool(record), tool, tool_length + 1);
    memcpy(record_arguments(record), arguments, arguments_length + 1);
    s_used += record_size;
    *id = record->id;
    return ESP_OK;
}

void mp_confirmation_reset(void)
{
    memset(s_arena, 0, s_used);
    s_used = 0;
    s_next_id = 1000;
}

esp_err_t mp_confirmation_take(const char *chat_id, uint32_t id, char *tool, size_t tool_size,
                               char *arguments, size_t arguments_size)
{
    if (!chat_id || !tool || !arguments) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t offset;
    if (!find_record(chat_id, id, &offset)) {
        return ESP_ERR_NOT_FOUND;
    }
    pending_header_t *record = record_at(offset);
    if (record->tool_length >= tool_size || record->arguments_length >= arguments_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(tool, record_tool(record), record->tool_length + 1);
    memcpy(arguments, record_arguments(record), record->arguments_length + 1);
    remove_record(offset);
    return ESP_OK;
}

esp_err_t mp_confirmation_cancel(const char *chat_id, uint32_t id)
{
    if (!chat_id) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t offset;
    if (!find_record(chat_id, id, &offset)) {
        return ESP_ERR_NOT_FOUND;
    }
    remove_record(offset);
    return ESP_OK;
}
