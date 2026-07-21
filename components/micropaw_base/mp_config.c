#include "mp_config.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

typedef struct {
    const char *name;
    const char *nvs_key;
    char *value;
    size_t size;
    bool secret;
} config_field_t;

static mp_config_t s_config;

static size_t field_count(void);
static config_field_t field_at(size_t index);
static bool valid_value(config_field_t field, const char *value);
static esp_err_t load_value(nvs_handle_t handle, config_field_t field);
static void append_field(char *output, size_t size, config_field_t field);
esp_err_t mp_config_init(void);
const mp_config_t *mp_config_get(void);
esp_err_t mp_config_set(const char *key, const char *value);
esp_err_t mp_config_erase(void);
void mp_config_format(char *output, size_t size);

static size_t field_count(void)
{
    return 12;
}

static config_field_t field_at(size_t index)
{
    config_field_t fields[] = {
        {"wifi_ssid", "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid), false},
        {"wifi_password", "wifi_pass", s_config.wifi_password, sizeof(s_config.wifi_password), true},
        {"telegram_token", "tg_token", s_config.telegram_token, sizeof(s_config.telegram_token), true},
        {"owner_chat_id", "owner_chat", s_config.owner_chat_id, sizeof(s_config.owner_chat_id), false},
        {"llm_provider", "llm_provider", s_config.llm_provider, sizeof(s_config.llm_provider), false},
        {"llm_api_key", "llm_key", s_config.llm_api_key, sizeof(s_config.llm_api_key), true},
        {"llm_model", "model", s_config.llm_model, sizeof(s_config.llm_model), false},
        {"llm_endpoint", "llm_endpoint", s_config.llm_endpoint, sizeof(s_config.llm_endpoint), false},
        {"google_client_id", "google_id", s_config.google_client_id, sizeof(s_config.google_client_id), true},
        {"google_client_secret", "google_sec", s_config.google_client_secret, sizeof(s_config.google_client_secret), true},
        {"google_refresh_token", "google_ref", s_config.google_refresh_token, sizeof(s_config.google_refresh_token), true},
        {"timezone", "timezone", s_config.timezone, sizeof(s_config.timezone), false}
    };
    return fields[index];
}

static bool valid_value(config_field_t field, const char *value)
{
    if (strcmp(field.name, "llm_provider") == 0) {
        return strcmp(value, "openai") == 0 || strcmp(value, "openrouter") == 0 ||
               strcmp(value, "openai_compatible") == 0;
    }
    if (strcmp(field.name, "llm_endpoint") == 0) {
        return !value[0] || strncmp(value, "https://", 8) == 0;
    }
    return true;
}

static esp_err_t load_value(nvs_handle_t handle, config_field_t field)
{
    size_t size = field.size;
    esp_err_t error = nvs_get_str(handle, field.nvs_key, field.value, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return error;
}

static void append_field(char *output, size_t size, config_field_t field)
{
    size_t used = strnlen(output, size);
    const char *value = field.value[0] ? field.value : "(unset)";
    if (field.secret && field.value[0]) {
        value = "(set)";
    }
    if (used < size) {
        snprintf(output + used, size - used, "%s=%s\n", field.name, value);
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
    memset(&s_config, 0, sizeof(s_config));
    strlcpy(s_config.llm_provider, "openai", sizeof(s_config.llm_provider));
    strlcpy(s_config.llm_model, CONFIG_MICROPAW_LLM_MODEL, sizeof(s_config.llm_model));
    strlcpy(s_config.timezone, "UTC0", sizeof(s_config.timezone));
    error = nvs_open("mp_config", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    for (size_t index = 0; index < field_count() && error == ESP_OK; index++) {
        error = load_value(handle, field_at(index));
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
    config_field_t field = {0};
    bool found = false;
    for (size_t index = 0; index < field_count(); index++) {
        field = field_at(index);
        if (strcmp(key, field.name) == 0) {
            found = true;
            break;
        }
    }
    if (!found || strlen(value) >= field.size || !valid_value(field, value)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = nvs_open("mp_config", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, field.nvs_key, value);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    if (error == ESP_OK) {
        strlcpy(field.value, value, field.size);
    }
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
        memset(&s_config, 0, sizeof(s_config));
        strlcpy(s_config.llm_provider, "openai", sizeof(s_config.llm_provider));
        strlcpy(s_config.llm_model, CONFIG_MICROPAW_LLM_MODEL, sizeof(s_config.llm_model));
        strlcpy(s_config.timezone, "UTC0", sizeof(s_config.timezone));
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
        append_field(output, size, field_at(index));
    }
}
