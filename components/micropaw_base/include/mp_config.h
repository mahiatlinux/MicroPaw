#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define MP_CONFIG_TOML_MAX 4096

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char telegram_token[64];
    char owner_chat_id[24];
    char llm_provider[20];
    char llm_api_key[256];
    char llm_model[96];
    char llm_endpoint[256];
    char llm_max_output_tokens[6];
    char transcription_model[96];
    char personality[769];
    char google_client_id[128];
    char google_client_secret[96];
    char google_refresh_token[256];
    char instagram_enabled[6];
    char zernio_api_key[96];
    char instagram_owner_username[32];
    char email_permission[11];
    char calendar_permission[11];
    char timezone[64];
    char morning_briefing_enabled[6];
    char morning_briefing_time[6];
    char oled_enabled[6];
    char oled_height[3];
} mp_config_t;

esp_err_t mp_config_init(void);
const mp_config_t *mp_config_get(void);
esp_err_t mp_config_set(const char *key, const char *value);
esp_err_t mp_config_import_toml(const char *text, size_t length, size_t *error_line);
esp_err_t mp_config_erase(void);
void mp_config_format(char *output, size_t size);
