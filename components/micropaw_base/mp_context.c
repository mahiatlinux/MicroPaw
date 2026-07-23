#include "mp_context.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define CONTEXT_MAGIC 0x4d504333U

typedef struct {
    uint32_t magic;
    uint32_t generation;
    uint32_t first_sequence;
    uint32_t next_sequence;
    uint32_t turn_count;
    uint32_t recent_bytes;
    uint32_t summary_bytes;
    uint32_t summary_bank;
    uint32_t compaction_count;
    uint32_t compaction_failures;
    uint32_t last_compaction_ms;
    uint32_t checksum;
} context_meta_t;

static context_meta_t s_meta;

static uint32_t meta_checksum(const context_meta_t *meta);
static bool meta_valid(const context_meta_t *meta);
static void fresh_meta(context_meta_t *meta);
static void meta_key(uint32_t bank, char *key, size_t size);
static void summary_key(uint32_t bank, char *key, size_t size);
static void turn_key(uint32_t sequence, char *key, size_t size);
static esp_err_t open_context(nvs_open_mode_t mode, nvs_handle_t *handle);
static esp_err_t read_meta(nvs_handle_t handle, uint32_t bank, context_meta_t *meta);
static esp_err_t write_meta(nvs_handle_t handle, context_meta_t *meta);
static esp_err_t blob_size(nvs_handle_t handle, const char *key, size_t *size);
static bool append_blob(nvs_handle_t handle, const char *key, mp_writer_t *writer);
static esp_err_t removed_bytes(nvs_handle_t handle, uint32_t count, size_t *bytes);
static bool format_range(mp_writer_t *writer, uint32_t first, uint32_t count,
                         bool include_summary);
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

static uint32_t meta_checksum(const context_meta_t *meta)
{
    const uint32_t *words = (const uint32_t *)meta;
    uint32_t checksum = 0x85a93f17U;
    for (size_t index = 0; index < (sizeof(*meta) / sizeof(uint32_t)) - 1; index++) {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= words[index];
    }
    return checksum;
}

static bool meta_valid(const context_meta_t *meta)
{
    return meta->magic == CONTEXT_MAGIC && meta->first_sequence <= meta->next_sequence &&
           meta->turn_count == meta->next_sequence - meta->first_sequence &&
           meta->summary_bank <= 1 && meta->checksum == meta_checksum(meta);
}

static void fresh_meta(context_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    meta->magic = CONTEXT_MAGIC;
    meta->first_sequence = 1;
    meta->next_sequence = 1;
    meta->checksum = meta_checksum(meta);
}

static void meta_key(uint32_t bank, char *key, size_t size)
{
    snprintf(key, size, "meta%lu", (unsigned long)bank);
}

static void summary_key(uint32_t bank, char *key, size_t size)
{
    snprintf(key, size, "summary%lu", (unsigned long)bank);
}

static void turn_key(uint32_t sequence, char *key, size_t size)
{
    snprintf(key, size, "t%08lx", (unsigned long)sequence);
}

static esp_err_t open_context(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open_from_partition("context", "conversation", mode, handle);
}

static esp_err_t read_meta(nvs_handle_t handle, uint32_t bank, context_meta_t *meta)
{
    char key[16];
    size_t size = sizeof(*meta);
    meta_key(bank, key, sizeof(key));
    esp_err_t error = nvs_get_blob(handle, key, meta, &size);
    return error == ESP_OK && size == sizeof(*meta) && meta_valid(meta) ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t write_meta(nvs_handle_t handle, context_meta_t *meta)
{
    char key[16];
    meta->checksum = meta_checksum(meta);
    meta_key(meta->generation & 1U, key, sizeof(key));
    esp_err_t error = nvs_set_blob(handle, key, meta, sizeof(*meta));
    return error == ESP_OK ? nvs_commit(handle) : error;
}

static esp_err_t blob_size(nvs_handle_t handle, const char *key, size_t *size)
{
    *size = 0;
    return nvs_get_blob(handle, key, NULL, size);
}

static bool append_blob(nvs_handle_t handle, const char *key, mp_writer_t *writer)
{
    size_t size;
    if (blob_size(handle, key, &size) != ESP_OK || size >= writer->size - writer->length) {
        writer->valid = false;
        return false;
    }
    esp_err_t error = nvs_get_blob(handle, key, writer->data + writer->length, &size);
    if (error != ESP_OK) {
        writer->valid = false;
        return false;
    }
    writer->length += size;
    writer->data[writer->length] = 0;
    return true;
}

static esp_err_t removed_bytes(nvs_handle_t handle, uint32_t count, size_t *bytes)
{
    *bytes = 0;
    for (uint32_t index = 0; index < count; index++) {
        char key[16];
        size_t size;
        turn_key(s_meta.first_sequence + index, key, sizeof(key));
        esp_err_t error = blob_size(handle, key, &size);
        if (error != ESP_OK) {
            return error;
        }
        *bytes += size;
    }
    return ESP_OK;
}

static bool format_range(mp_writer_t *writer, uint32_t first, uint32_t count,
                         bool include_summary)
{
    nvs_handle_t handle;
    if (open_context(NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    bool wrote = false;
    if (include_summary && s_meta.summary_bytes) {
        char key[16];
        summary_key(s_meta.summary_bank, key, sizeof(key));
        if (!append_blob(handle, key, writer)) {
            nvs_close(handle);
            return false;
        }
        wrote = true;
    }
    for (uint32_t index = 0; index < count; index++) {
        char key[16];
        if (wrote) {
            mp_writer_char(writer, ',');
        }
        turn_key(first + index, key, sizeof(key));
        if (!append_blob(handle, key, writer)) {
            nvs_close(handle);
            return false;
        }
        wrote = true;
    }
    nvs_close(handle);
    return wrote;
}

esp_err_t mp_context_init(void)
{
    esp_err_t error = nvs_flash_init_partition("context");
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase_partition("context");
        if (error == ESP_OK) {
            error = nvs_flash_init_partition("context");
        }
    }
    if (error != ESP_OK) {
        return error;
    }
    nvs_handle_t handle;
    error = open_context(NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    context_meta_t first;
    context_meta_t second;
    bool first_valid = read_meta(handle, 0, &first) == ESP_OK;
    bool second_valid = read_meta(handle, 1, &second) == ESP_OK;
    if (first_valid || second_valid) {
        s_meta = !second_valid || (first_valid && first.generation >= second.generation) ?
                 first : second;
    } else {
        fresh_meta(&s_meta);
        error = write_meta(handle, &s_meta);
    }
    nvs_close(handle);
    return error;
}

esp_err_t mp_context_append(const char *items, size_t size)
{
    if (!items || !size || size > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t error = open_context(NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    char key[16];
    turn_key(s_meta.next_sequence, key, sizeof(key));
    error = nvs_set_blob(handle, key, items, size);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (error == ESP_OK) {
        context_meta_t next = s_meta;
        next.generation++;
        next.next_sequence++;
        next.turn_count++;
        next.recent_bytes += size;
        error = write_meta(handle, &next);
        if (error == ESP_OK) {
            s_meta = next;
        }
    }
    nvs_close(handle);
    return error;
}

bool mp_context_format(mp_writer_t *writer)
{
    return format_range(writer, s_meta.first_sequence, s_meta.turn_count, true);
}

bool mp_context_compaction_plan(mp_context_compaction_t *plan)
{
    if (!plan) {
        return false;
    }
    memset(plan, 0, sizeof(*plan));
    uint32_t remove_count = s_meta.turn_count >= MP_CONTEXT_TURN_TRIGGER ? 3 : 0;
    nvs_handle_t handle;
    if (open_context(NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t removed = 0;
    if (s_meta.recent_bytes >= MP_CONTEXT_BYTE_TRIGGER) {
        remove_count = 0;
        size_t remaining = s_meta.recent_bytes;
        while (remaining >= MP_CONTEXT_BYTE_TARGET &&
               s_meta.turn_count - remove_count > 2) {
            char key[16];
            size_t size;
            turn_key(s_meta.first_sequence + remove_count, key, sizeof(key));
            if (blob_size(handle, key, &size) != ESP_OK) {
                nvs_close(handle);
                return false;
            }
            remaining -= size;
            removed += size;
            remove_count++;
        }
    }
    if (remove_count && !removed &&
        removed_bytes(handle, remove_count, &removed) != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    nvs_close(handle);
    plan->remove_count = remove_count;
    plan->source_bytes = removed + s_meta.summary_bytes;
    return remove_count > 0;
}

bool mp_context_format_compaction(mp_writer_t *writer, uint32_t remove_count)
{
    if (!remove_count || remove_count > s_meta.turn_count) {
        return false;
    }
    return format_range(writer, s_meta.first_sequence, remove_count, true);
}

esp_err_t mp_context_commit_summary(const char *summary_item, size_t size,
                                    uint32_t remove_count, uint32_t elapsed_ms)
{
    if (!summary_item || !size || remove_count > s_meta.turn_count || size > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t error = open_context(NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    uint32_t new_bank = s_meta.summary_bank ^ 1U;
    char key[16];
    summary_key(new_bank, key, sizeof(key));
    error = nvs_set_blob(handle, key, summary_item, size);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    size_t removed = 0;
    if (error == ESP_OK) {
        error = removed_bytes(handle, remove_count, &removed);
    }
    context_meta_t next = s_meta;
    if (error == ESP_OK) {
        next.generation++;
        next.first_sequence += remove_count;
        next.turn_count -= remove_count;
        next.recent_bytes -= removed;
        next.summary_bank = new_bank;
        next.summary_bytes = size;
        next.compaction_count++;
        next.last_compaction_ms = elapsed_ms;
        error = write_meta(handle, &next);
    }
    if (error == ESP_OK) {
        for (uint32_t index = 0; index < remove_count; index++) {
            turn_key(s_meta.first_sequence + index, key, sizeof(key));
            nvs_erase_key(handle, key);
        }
        if (s_meta.summary_bytes) {
            summary_key(s_meta.summary_bank, key, sizeof(key));
            nvs_erase_key(handle, key);
        }
        nvs_commit(handle);
        s_meta = next;
    }
    nvs_close(handle);
    return error;
}

esp_err_t mp_context_compaction_failed(uint32_t elapsed_ms)
{
    nvs_handle_t handle;
    esp_err_t error = open_context(NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    context_meta_t next = s_meta;
    next.generation++;
    next.compaction_failures++;
    next.last_compaction_ms = elapsed_ms;
    error = write_meta(handle, &next);
    if (error == ESP_OK) {
        s_meta = next;
    }
    nvs_close(handle);
    return error;
}

esp_err_t mp_context_forget(void)
{
    nvs_handle_t handle;
    esp_err_t error = open_context(NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_all(handle);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (error == ESP_OK) {
        fresh_meta(&s_meta);
        error = write_meta(handle, &s_meta);
    }
    nvs_close(handle);
    return error;
}

size_t mp_context_stored_bytes(void)
{
    return s_meta.recent_bytes + s_meta.summary_bytes;
}

uint32_t mp_context_turn_count(void)
{
    return s_meta.turn_count;
}

uint32_t mp_context_compaction_count(void)
{
    return s_meta.compaction_count;
}

uint32_t mp_context_compaction_failures(void)
{
    return s_meta.compaction_failures;
}

uint32_t mp_context_last_compaction_ms(void)
{
    return s_meta.last_compaction_ms;
}
