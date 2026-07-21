#include "mp_json.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *skip_space(const char *cursor, const char *end);
static const char *string_end(const char *cursor, const char *end);
static const char *value_end(const char *cursor, const char *end);
static int hex_digit(char value);
static bool append_utf8(char *output, size_t size, size_t *used, uint32_t value);
static bool decode_string(const char *value, size_t length, char *output, size_t size);

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

static const char *skip_space(const char *cursor, const char *end)
{
    while (cursor < end && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static const char *string_end(const char *cursor, const char *end)
{
    if (cursor >= end || *cursor != '"') {
        return NULL;
    }
    for (cursor++; cursor < end; cursor++) {
        if (*cursor == '\\') {
            cursor++;
        } else if (*cursor == '"') {
            return cursor + 1;
        }
    }
    return NULL;
}

static const char *value_end(const char *cursor, const char *end)
{
    cursor = skip_space(cursor, end);
    if (cursor >= end) {
        return NULL;
    }
    if (*cursor == '"') {
        return string_end(cursor, end);
    }
    if (*cursor == '{' || *cursor == '[') {
        int depth = 0;
        bool quoted = false;
        for (const char *scan = cursor; scan < end; scan++) {
            if (quoted) {
                if (*scan == '\\') {
                    scan++;
                } else if (*scan == '"') {
                    quoted = false;
                }
            } else if (*scan == '"') {
                quoted = true;
            } else if (*scan == '{' || *scan == '[') {
                depth++;
            } else if (*scan == '}' || *scan == ']') {
                if (--depth == 0) {
                    return scan + 1;
                }
            }
        }
        return NULL;
    }
    const char *scan = cursor;
    while (scan < end && *scan != ',' && *scan != '}' && *scan != ']') {
        scan++;
    }
    while (scan > cursor && isspace((unsigned char)scan[-1])) {
        scan--;
    }
    return scan;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool append_utf8(char *output, size_t size, size_t *used, uint32_t value)
{
    size_t needed = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
    if (*used + needed >= size) {
        return false;
    }
    if (needed == 1) {
        output[(*used)++] = (char)value;
    } else if (needed == 2) {
        output[(*used)++] = (char)(0xc0 | (value >> 6));
        output[(*used)++] = (char)(0x80 | (value & 0x3f));
    } else if (needed == 3) {
        output[(*used)++] = (char)(0xe0 | (value >> 12));
        output[(*used)++] = (char)(0x80 | ((value >> 6) & 0x3f));
        output[(*used)++] = (char)(0x80 | (value & 0x3f));
    } else {
        output[(*used)++] = (char)(0xf0 | (value >> 18));
        output[(*used)++] = (char)(0x80 | ((value >> 12) & 0x3f));
        output[(*used)++] = (char)(0x80 | ((value >> 6) & 0x3f));
        output[(*used)++] = (char)(0x80 | (value & 0x3f));
    }
    output[*used] = 0;
    return true;
}

static bool decode_string(const char *value, size_t length, char *output, size_t size)
{
    if (length < 2 || value[0] != '"' || value[length - 1] != '"' || size == 0) {
        return false;
    }
    size_t used = 0;
    output[0] = 0;
    for (size_t index = 1; index + 1 < length; index++) {
        unsigned char current = (unsigned char)value[index];
        if (current != '\\') {
            if (used + 1 >= size) {
                return false;
            }
            output[used++] = (char)current;
            output[used] = 0;
            continue;
        }
        if (++index + 1 >= length) {
            return false;
        }
        char escaped = value[index];
        if (escaped == 'u') {
            if (index + 4 >= length) {
                return false;
            }
            uint32_t code = 0;
            for (int digit = 0; digit < 4; digit++) {
                int hex = hex_digit(value[++index]);
                if (hex < 0) {
                    return false;
                }
                code = (code << 4) | (uint32_t)hex;
            }
            if (code >= 0xd800 && code <= 0xdbff && index + 6 < length &&
                value[index + 1] == '\\' && value[index + 2] == 'u') {
                uint32_t low = 0;
                index += 2;
                for (int digit = 0; digit < 4; digit++) {
                    int hex = hex_digit(value[++index]);
                    if (hex < 0) {
                        return false;
                    }
                    low = (low << 4) | (uint32_t)hex;
                }
                if (low >= 0xdc00 && low <= 0xdfff) {
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                }
            }
            if (!append_utf8(output, size, &used, code)) {
                return false;
            }
        } else {
            char decoded = escaped == 'n' ? '\n' : escaped == 'r' ? '\r' :
                           escaped == 't' ? '\t' : escaped == 'b' ? '\b' :
                           escaped == 'f' ? '\f' : escaped;
            if (used + 1 >= size) {
                return false;
            }
            output[used++] = decoded;
            output[used] = 0;
        }
    }
    return true;
}

void mp_writer_init(mp_writer_t *writer, char *data, size_t size)
{
    writer->data = data;
    writer->size = size;
    writer->length = 0;
    writer->valid = size > 0;
    if (size > 0) {
        data[0] = 0;
    }
}

bool mp_writer_raw(mp_writer_t *writer, const char *text)
{
    size_t length = strlen(text);
    if (!writer->valid || writer->length + length >= writer->size) {
        writer->valid = false;
        return false;
    }
    memcpy(writer->data + writer->length, text, length + 1);
    writer->length += length;
    return true;
}

bool mp_writer_char(mp_writer_t *writer, char value)
{
    if (!writer->valid || writer->length + 1 >= writer->size) {
        writer->valid = false;
        return false;
    }
    writer->data[writer->length++] = value;
    writer->data[writer->length] = 0;
    return true;
}

bool mp_writer_string(mp_writer_t *writer, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *cursor = (const unsigned char *)text;
    mp_writer_char(writer, '"');
    while (*cursor && writer->valid) {
        unsigned char value = *cursor++;
        if (value == '"' || value == '\\') {
            mp_writer_char(writer, '\\');
            mp_writer_char(writer, (char)value);
        } else if (value == '\n') {
            mp_writer_raw(writer, "\\n");
        } else if (value == '\r') {
            mp_writer_raw(writer, "\\r");
        } else if (value == '\t') {
            mp_writer_raw(writer, "\\t");
        } else if (value < 0x20) {
            mp_writer_raw(writer, "\\u00");
            mp_writer_char(writer, hex[value >> 4]);
            mp_writer_char(writer, hex[value & 15]);
        } else {
            mp_writer_char(writer, (char)value);
        }
    }
    mp_writer_char(writer, '"');
    return writer->valid;
}

bool mp_writer_format(mp_writer_t *writer, const char *format, ...)
{
    va_list args;
    int written;
    if (!writer->valid) {
        return false;
    }
    va_start(args, format);
    written = vsnprintf(writer->data + writer->length, writer->size - writer->length, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= writer->size - writer->length) {
        writer->valid = false;
        return false;
    }
    writer->length += (size_t)written;
    return true;
}

size_t mp_url_encode(const char *source, char *target, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t offset = 0;
    while (*source && offset + 1 < size) {
        unsigned char value = (unsigned char)*source++;
        bool plain = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                     (value >= '0' && value <= '9') || value == '-' || value == '_' ||
                     value == '.' || value == '~';
        if (plain) {
            target[offset++] = (char)value;
        } else if (offset + 3 < size) {
            target[offset++] = '%';
            target[offset++] = hex[value >> 4];
            target[offset++] = hex[value & 15];
        } else {
            break;
        }
    }
    target[offset] = 0;
    return offset;
}

bool mp_json_get_slice(const char *json, size_t length, const char *key,
                       const char **value, size_t *value_length)
{
    if (!json || !key || !value || !value_length) {
        return false;
    }
    const char *end = json + length;
    const char *cursor = skip_space(json, end);
    if (cursor >= end || *cursor++ != '{') {
        return false;
    }
    while ((cursor = skip_space(cursor, end)) < end && *cursor != '}') {
        const char *key_end = string_end(cursor, end);
        if (!key_end) {
            return false;
        }
        bool match = (size_t)(key_end - cursor) == strlen(key) + 2 &&
                     memcmp(cursor + 1, key, strlen(key)) == 0;
        cursor = skip_space(key_end, end);
        if (cursor >= end || *cursor++ != ':') {
            return false;
        }
        cursor = skip_space(cursor, end);
        const char *item_end = value_end(cursor, end);
        if (!item_end) {
            return false;
        }
        if (match) {
            *value = cursor;
            *value_length = item_end - cursor;
            return true;
        }
        cursor = skip_space(item_end, end);
        if (cursor < end && *cursor == ',') {
            cursor++;
        }
    }
    return false;
}

bool mp_json_get_string(const char *json, size_t length, const char *key,
                        char *output, size_t size)
{
    const char *value;
    size_t value_length;
    return mp_json_get_slice(json, length, key, &value, &value_length) &&
           decode_string(value, value_length, output, size);
}

bool mp_json_get_int64(const char *json, size_t length, const char *key, int64_t *value)
{
    const char *text;
    size_t text_length;
    char number[32];
    if (!value || !mp_json_get_slice(json, length, key, &text, &text_length) ||
        text_length == 0 || text_length >= sizeof(number)) {
        return false;
    }
    memcpy(number, text, text_length);
    number[text_length] = 0;
    char *end;
    long long parsed = strtoll(number, &end, 10);
    if (*end) {
        return false;
    }
    *value = parsed;
    return true;
}

bool mp_json_get_bool(const char *json, size_t length, const char *key, bool *value)
{
    const char *text;
    size_t text_length;
    if (!value || !mp_json_get_slice(json, length, key, &text, &text_length)) {
        return false;
    }
    if (text_length == 4 && memcmp(text, "true", 4) == 0) {
        *value = true;
        return true;
    }
    if (text_length == 5 && memcmp(text, "false", 5) == 0) {
        *value = false;
        return true;
    }
    return false;
}

bool mp_json_first(const char *array, size_t length, const char **value, size_t *value_length)
{
    if (!array || !value || !value_length) {
        return false;
    }
    const char *end = array + length;
    const char *cursor = skip_space(array, end);
    if (cursor >= end || *cursor++ != '[') {
        return false;
    }
    cursor = skip_space(cursor, end);
    if (cursor >= end || *cursor == ']') {
        return false;
    }
    const char *item_end = value_end(cursor, end);
    if (!item_end) {
        return false;
    }
    *value = cursor;
    *value_length = item_end - cursor;
    return true;
}
