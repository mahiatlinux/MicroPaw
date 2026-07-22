#include "mp_config.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

typedef struct {
    const char *name;
    const char *nvs_key;
    size_t offset;
    size_t size;
    bool secret;
} config_field_t;

static mp_config_t s_config;
EXT_RAM_BSS_ATTR static mp_config_t s_import;
static const config_field_t s_fields[] = {
    {"wifi_ssid", "wifi_ssid", offsetof(mp_config_t, wifi_ssid), sizeof(s_config.wifi_ssid), false},
    {"wifi_password", "wifi_pass", offsetof(mp_config_t, wifi_password), sizeof(s_config.wifi_password), true},
    {"telegram_token", "tg_token", offsetof(mp_config_t, telegram_token), sizeof(s_config.telegram_token), true},
    {"owner_chat_id", "owner_chat", offsetof(mp_config_t, owner_chat_id), sizeof(s_config.owner_chat_id), false},
    {"llm_provider", "llm_provider", offsetof(mp_config_t, llm_provider), sizeof(s_config.llm_provider), false},
    {"llm_api_key", "llm_key", offsetof(mp_config_t, llm_api_key), sizeof(s_config.llm_api_key), true},
    {"llm_model", "model", offsetof(mp_config_t, llm_model), sizeof(s_config.llm_model), false},
    {"llm_endpoint", "llm_endpoint", offsetof(mp_config_t, llm_endpoint), sizeof(s_config.llm_endpoint), false},
    {"llm_max_output_tokens", "llm_tokens", offsetof(mp_config_t, llm_max_output_tokens), sizeof(s_config.llm_max_output_tokens), false},
    {"google_client_id", "google_id", offsetof(mp_config_t, google_client_id), sizeof(s_config.google_client_id), true},
    {"google_client_secret", "google_sec", offsetof(mp_config_t, google_client_secret), sizeof(s_config.google_client_secret), true},
    {"google_refresh_token", "google_ref", offsetof(mp_config_t, google_refresh_token), sizeof(s_config.google_refresh_token), true},
    {"email_permission", "email_perm", offsetof(mp_config_t, email_permission), sizeof(s_config.email_permission), false},
    {"calendar_permission", "calendar_perm", offsetof(mp_config_t, calendar_permission), sizeof(s_config.calendar_permission), false},
    {"timezone", "timezone", offsetof(mp_config_t, timezone), sizeof(s_config.timezone), false}
};

static void config_defaults(void);
static size_t field_count(void);
static char *field_value(mp_config_t *config, const config_field_t *field);
static const char *const_field_value(const mp_config_t *config, const config_field_t *field);
static int field_index(const char *name, size_t length);
static bool valid_value(config_field_t field, const char *value);
static bool parse_toml_string(const char **cursor, const char *end, char *output, size_t size);
static esp_err_t parse_toml(const char *text, size_t length, uint16_t *present, size_t *error_line);
static esp_err_t load_value(nvs_handle_t handle, const config_field_t *field);
static void append_field(char *output, size_t size, const config_field_t *field);
esp_err_t mp_config_init(void);
const mp_config_t *mp_config_get(void);
esp_err_t mp_config_set(const char *key, const char *value);
esp_err_t mp_config_import_toml(const char *text, size_t length, size_t *error_line);
esp_err_t mp_config_erase(void);
void mp_config_format(char *output, size_t size);

static void config_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    strlcpy(s_config.llm_provider, "openai", sizeof(s_config.llm_provider));
    strlcpy(s_config.llm_model, CONFIG_MICROPAW_LLM_MODEL, sizeof(s_config.llm_model));
    snprintf(s_config.llm_max_output_tokens, sizeof(s_config.llm_max_output_tokens), "%d",
             CONFIG_MICROPAW_LLM_MAX_OUTPUT_TOKENS);
    strlcpy(s_config.email_permission, "permission", sizeof(s_config.email_permission));
    strlcpy(s_config.calendar_permission, "permission", sizeof(s_config.calendar_permission));
    strlcpy(s_config.timezone, "UTC0", sizeof(s_config.timezone));
}

static size_t field_count(void)
{
    return sizeof(s_fields) / sizeof(s_fields[0]);
}

static char *field_value(mp_config_t *config, const config_field_t *field)
{
    return (char *)config + field->offset;
}

static const char *const_field_value(const mp_config_t *config, const config_field_t *field)
{
    return (const char *)config + field->offset;
}

static int field_index(const char *name, size_t length)
{
    for (size_t index = 0; index < field_count(); index++) {
        if (strlen(s_fields[index].name) == length && strncmp(name, s_fields[index].name, length) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static bool valid_value(config_field_t field, const char *value)
{
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 0x20 || *cursor == 0x7f) {
            return false;
        }
    }
    if (strcmp(field.name, "llm_provider") == 0) {
        return strcmp(value, "openai") == 0 || strcmp(value, "openrouter") == 0 ||
               strcmp(value, "openai_compatible") == 0;
    }
    if (strcmp(field.name, "llm_endpoint") == 0) {
        return !value[0] || strncmp(value, "https://", 8) == 0;
    }
    if (strcmp(field.name, "llm_max_output_tokens") == 0) {
        char *end;
        unsigned long parsed = strtoul(value, &end, 10);
        return value[0] && !*end && parsed >= 1024 &&
               parsed <= CONFIG_MICROPAW_LLM_MAX_OUTPUT_TOKENS;
    }
    if (strcmp(field.name, "email_permission") == 0 ||
        strcmp(field.name, "calendar_permission") == 0) {
        return strcmp(value, "allowed") == 0 || strcmp(value, "permission") == 0 ||
               strcmp(value, "disabled") == 0;
    }
    return true;
}

static bool parse_toml_string(const char **cursor, const char *end, char *output, size_t size)
{
    if (*cursor == end || (**cursor != '"' && **cursor != '\'')) {
        return false;
    }
    char quote = *(*cursor)++;
    size_t used = 0;
    while (*cursor < end) {
        char value = *(*cursor)++;
        if (value == quote) {
            output[used] = 0;
            return true;
        }
        if (quote == '"' && value == '\\') {
            if (*cursor == end) {
                return false;
            }
            value = *(*cursor)++;
            if (value != '"' && value != '\\') {
                return false;
            }
        }
        if (used + 1 >= size) {
            return false;
        }
        output[used++] = value;
    }
    return false;
}

static esp_err_t parse_toml(const char *text, size_t length, uint16_t *present, size_t *error_line)
{
    const char *cursor = text;
    const char *limit = text + length;
    size_t line = 1;
    *present = 0;
    while (cursor < limit) {
        const char *end = memchr(cursor, '\n', limit - cursor);
        if (!end) {
            end = limit;
        }
        const char *line_end = end;
        if (line_end > cursor && line_end[-1] == '\r') {
            line_end--;
        }
        const char *value = cursor;
        while (value < line_end && (*value == ' ' || *value == '\t')) {
            value++;
        }
        if (value < line_end && *value != '#') {
            const char *key = value;
            while (value < line_end && (isalnum((unsigned char)*value) || *value == '_')) {
                value++;
            }
            size_t key_length = value - key;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            int index = field_index(key, key_length);
            if (!key_length || value == line_end || *value++ != '=' || index < 0 ||
                (*present & (1U << index))) {
                *error_line = line;
                return ESP_ERR_INVALID_ARG;
            }
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            char *destination = field_value(&s_import, &s_fields[index]);
            if (!parse_toml_string(&value, line_end, destination, s_fields[index].size)) {
                *error_line = line;
                return ESP_ERR_INVALID_ARG;
            }
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            if ((value < line_end && *value != '#') ||
                !valid_value(s_fields[index], destination)) {
                *error_line = line;
                return ESP_ERR_INVALID_ARG;
            }
            *present |= 1U << index;
        }
        cursor = end < limit ? end + 1 : end;
        line++;
    }
    if (!*present) {
        *error_line = 1;
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t load_value(nvs_handle_t handle, const config_field_t *field)
{
    size_t size = field->size;
    esp_err_t error = nvs_get_str(handle, field->nvs_key, field_value(&s_config, field), &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return error;
}

static void append_field(char *output, size_t size, const config_field_t *field)
{
    size_t used = strnlen(output, size);
    const char *field_text = const_field_value(&s_config, field);
    const char *value = field_text[0] ? field_text : "(unset)";
    if (field->secret && field_text[0]) {
        value = "(set)";
    }
    if (used < size) {
        snprintf(output + used, size - used, "%s=%s\n", field->name, value);
    }
}

esp_err_t mp_config_init(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    if (error != ESP_OK) {
        return error;
    }
    config_defaults();
    error = nvs_open("mp_config", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    for (size_t index = 0; index < field_count() && error == ESP_OK; index++) {
        error = load_value(handle, &s_fields[index]);
    }
    nvs_close(handle);
    return error;
}

const mp_config_t *mp_config_get(void)
{
    return &s_config;
}

esp_err_t mp_config_set(const char *key, const char *value)
{
    nvs_handle_t handle;
    int index = field_index(key, strlen(key));
    if (index < 0 || strlen(value) >= s_fields[index].size || !valid_value(s_fields[index], value)) {
        return ESP_ERR_INVALID_ARG;
    }
    const config_field_t *field = &s_fields[index];
    esp_err_t error = nvs_open("mp_config", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, field->nvs_key, value);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error == ESP_OK) {
        strlcpy(field_value(&s_config, field), value, field->size);
    }
    return error;
}

esp_err_t mp_config_import_toml(const char *text, size_t length, size_t *error_line)
{
    if (!text || !error_line || length == 0 || length > MP_CONFIG_TOML_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_import = s_config;
    uint16_t present;
    esp_err_t error = parse_toml(text, length, &present, error_line);
    if (error != ESP_OK) {
        memset(&s_import, 0, sizeof(s_import));
        return error;
    }
    nvs_handle_t handle;
    error = nvs_open("mp_config", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        *error_line = 0;
        memset(&s_import, 0, sizeof(s_import));
        return error;
    }
    for (size_t index = 0; index < field_count() && error == ESP_OK; index++) {
        if (present & (1U << index)) {
            error = nvs_set_str(handle, s_fields[index].nvs_key,
                                const_field_value(&s_import, &s_fields[index]));
        }
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error == ESP_OK) {
        s_config = s_import;
    } else {
        *error_line = 0;
    }
    memset(&s_import, 0, sizeof(s_import));
    return error;
}

esp_err_t mp_config_erase(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mp_config", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_all(handle);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error == ESP_OK) {
        config_defaults();
    }
    return error;
}

void mp_config_format(char *output, size_t size)
{
    if (size == 0) {
        return;
    }
    output[0] = 0;
    for (size_t index = 0; index < field_count(); index++) {
        append_field(output, size, &s_fields[index]);
    }
}
