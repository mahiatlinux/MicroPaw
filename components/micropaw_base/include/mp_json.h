#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t size;
    size_t length;
    bool valid;
} mp_writer_t;

void mp_writer_init(mp_writer_t *writer, char *data, size_t size);
bool mp_writer_raw(mp_writer_t *writer, const char *text);
bool mp_writer_char(mp_writer_t *writer, char value);
bool mp_writer_string(mp_writer_t *writer, const char *text);
bool mp_writer_format(mp_writer_t *writer, const char *format, ...);
size_t mp_url_encode(const char *source, char *target, size_t size);
bool mp_json_get_slice(const char *json, size_t length, const char *key,
                       const char **value, size_t *value_length);
bool mp_json_get_string(const char *json, size_t length, const char *key,
                        char *output, size_t size);
bool mp_json_get_int64(const char *json, size_t length, const char *key, int64_t *value);
bool mp_json_get_bool(const char *json, size_t length, const char *key, bool *value);
bool mp_json_first(const char *array, size_t length, const char **value, size_t *value_length);
