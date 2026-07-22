#include "mp_google.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "mp_config.h"
#include "mp_json.h"
#include "sdkconfig.h"

EXT_RAM_BSS_ATTR static char s_token[1536];
EXT_RAM_BSS_ATTR static char s_refresh_body[1024];
EXT_RAM_BSS_ATTR static char s_response[CONFIG_MICROPAW_WORK_TEXT_BYTES];
static time_t s_token_expiry;

static void google_error(char *output, size_t size, int status);
static esp_err_t google_token(const char **token, char *output, size_t size);
esp_err_t mp_google_request(esp_http_client_method_t method, const char *url,
                            const char *body, size_t body_size, mp_http_write_fn write,
                            void *write_context, int timeout_ms, const char *accepted_content_types,
                            mp_http_response_t *response, char *output, size_t size);
const char *mp_google_response(void);

static void google_error(char *output, size_t size, int status)
{
    const char *error;
    size_t length;
    size_t response_length = strlen(s_response);
    if (mp_json_get_string(s_response, response_length, "error_description", output, size)) {
        return;
    }
    if (mp_json_get_slice(s_response, response_length, "error", &error, &length) &&
        mp_json_get_string(error, length, "message", output, size)) {
        return;
    }
    snprintf(output, size, "Google HTTP status %d.", status);
}

static esp_err_t google_token(const char **token, char *output, size_t size)
{
    time_t now = time(NULL);
    if (s_token[0] && now + 60 < s_token_expiry) {
        *token = s_token;
        return ESP_OK;
    }
    const mp_config_t *config = mp_config_get();
    if (!config->google_client_id[0] || !config->google_refresh_token[0]) {
        strlcpy(output, "Google OAuth is not configured.", size);
        return ESP_ERR_INVALID_STATE;
    }
    char client_id[384];
    char secret[288];
    char refresh[768];
    mp_url_encode(config->google_client_id, client_id, sizeof(client_id));
    mp_url_encode(config->google_client_secret, secret, sizeof(secret));
    mp_url_encode(config->google_refresh_token, refresh, sizeof(refresh));
    mp_writer_t writer;
    mp_writer_init(&writer, s_refresh_body, sizeof(s_refresh_body));
    mp_writer_format(&writer, "client_id=%s&refresh_token=%s&grant_type=refresh_token", client_id, refresh);
    if (config->google_client_secret[0]) {
        mp_writer_format(&writer, "&client_secret=%s", secret);
    }
    if (!writer.valid) {
        return ESP_ERR_INVALID_SIZE;
    }
    mp_http_header_t headers[] = {{"Content-Type", "application/x-www-form-urlencoded"}};
    mp_http_request_t request = {
        .url = "https://oauth2.googleapis.com/token",
        .method = HTTP_METHOD_POST,
        .headers = headers,
        .header_count = 1,
        .body = s_refresh_body,
        .body_size = writer.length,
        .response_limit = sizeof(s_response) - 1,
        .timeout_ms = 20000,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response;
    esp_err_t result = mp_http_collect(&request, s_response, sizeof(s_response), &response);
    if (result != ESP_OK || response.truncated || response.status != 200) {
        if (result == ESP_OK) {
            google_error(output, size, response.status);
        }
        return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    }
    int64_t expires = 3000;
    size_t response_length = strlen(s_response);
    if (!mp_json_get_string(s_response, response_length, "access_token", s_token, sizeof(s_token))) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    mp_json_get_int64(s_response, response_length, "expires_in", &expires);
    s_token_expiry = now + expires;
    *token = s_token;
    return ESP_OK;
}

esp_err_t mp_google_request(esp_http_client_method_t method, const char *url,
                            const char *body, size_t body_size, mp_http_write_fn write,
                            void *write_context, int timeout_ms, const char *accepted_content_types,
                            mp_http_response_t *response, char *output, size_t size)
{
    if (!url || !response || !output || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *token;
    esp_err_t result = google_token(&token, output, size);
    if (result != ESP_OK) {
        return result;
    }
    char authorization[1560];
    snprintf(authorization, sizeof(authorization), "Bearer %s", token);
    mp_http_header_t headers[] = {
        {"Authorization", authorization},
        {"Content-Type", "application/json"}
    };
    mp_http_request_t request = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = body_size || write ? 2 : 1,
        .body = body,
        .body_size = body_size,
        .write = write,
        .write_context = write_context,
        .response_limit = sizeof(s_response) - 1,
        .timeout_ms = timeout_ms,
        .accepted_content_types = accepted_content_types
    };
    result = mp_http_collect(&request, s_response, sizeof(s_response), response);
    if (result != ESP_OK) {
        return result;
    }
    if (response->truncated) {
        strlcpy(output, "Google response exceeded device capacity.", size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (response->status < 200 || response->status >= 300) {
        google_error(output, size, response->status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

const char *mp_google_response(void)
{
    return s_response;
}
