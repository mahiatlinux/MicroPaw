#include "mp_services.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "mbedtls/base64.h"
#include "mp_config.h"
#include "mp_json.h"
#include "mp_net.h"

EXT_RAM_BSS_ATTR static char s_token[1536];
EXT_RAM_BSS_ATTR static char s_refresh_body[1024];
EXT_RAM_BSS_ATTR static char s_raw_email[5120];
EXT_RAM_BSS_ATTR static char s_encoded_email[6912];
EXT_RAM_BSS_ATTR static char s_request_body[7168];
EXT_RAM_BSS_ATTR static char s_response[2048];
static time_t s_token_expiry;

static void google_error(char *output, size_t size, int status);
static esp_err_t google_token(const char **token, char *output, size_t size);
static esp_err_t google_email(const mp_email_t *email, char *output, size_t size);
static esp_err_t google_calendar(const mp_calendar_event_t *event, char *output, size_t size);
static esp_err_t google_post(const char *url, const char *body, int *status, char *output, size_t size);
const mp_email_service_t *mp_email_service(void);
const mp_calendar_service_t *mp_calendar_service(void);

static const mp_email_service_t s_email = {"gmail", google_email};
static const mp_calendar_service_t s_calendar = {"google_calendar", google_calendar};

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
    mp_http_response_t response = {0};
    esp_err_t error = mp_http_collect(&request, s_response, sizeof(s_response), &response);
    if (error != ESP_OK || response.status != 200) {
        if (error == ESP_OK) {
            google_error(output, size, response.status);
        }
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    int64_t expires = 3000;
    if (!mp_json_get_string(s_response, strlen(s_response), "access_token", s_token, sizeof(s_token))) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    mp_json_get_int64(s_response, strlen(s_response), "expires_in", &expires);
    s_token_expiry = now + expires;
    *token = s_token;
    return ESP_OK;
}

static esp_err_t google_email(const mp_email_t *email, char *output, size_t size)
{
    if (!email || !email->to || !email->subject || !email->body) {
        return ESP_ERR_INVALID_ARG;
    }
    int length = snprintf(s_raw_email, sizeof(s_raw_email),
                          "To: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n%s",
                          email->to, email->subject, email->body);
    if (length < 0 || (size_t)length >= sizeof(s_raw_email)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t encoded = 0;
    if (mbedtls_base64_encode((unsigned char *)s_encoded_email, sizeof(s_encoded_email), &encoded,
                              (const unsigned char *)s_raw_email, length) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t index = 0; index < encoded; index++) {
        if (s_encoded_email[index] == '+') {
            s_encoded_email[index] = '-';
        } else if (s_encoded_email[index] == '/') {
            s_encoded_email[index] = '_';
        }
    }
    while (encoded && s_encoded_email[encoded - 1] == '=') {
        encoded--;
    }
    s_encoded_email[encoded] = 0;
    mp_writer_t writer;
    mp_writer_init(&writer, s_request_body, sizeof(s_request_body));
    mp_writer_raw(&writer, "{\"raw\":");
    mp_writer_string(&writer, s_encoded_email);
    mp_writer_char(&writer, '}');
    int status = 0;
    esp_err_t error = writer.valid ? google_post(
        "https://gmail.googleapis.com/gmail/v1/users/me/messages/send", s_request_body, &status,
        output, size) : ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK && status == 200) {
        strlcpy(output, "Email sent.", size);
        return ESP_OK;
    }
    return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
}

static esp_err_t google_calendar(const mp_calendar_event_t *event, char *output, size_t size)
{
    if (!event || !event->summary || !event->start_rfc3339 || !event->end_rfc3339) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_writer_t writer;
    mp_writer_init(&writer, s_request_body, sizeof(s_request_body));
    mp_writer_raw(&writer, "{\"summary\":");
    mp_writer_string(&writer, event->summary);
    mp_writer_raw(&writer, ",\"start\":{\"dateTime\":");
    mp_writer_string(&writer, event->start_rfc3339);
    mp_writer_raw(&writer, "},\"end\":{\"dateTime\":");
    mp_writer_string(&writer, event->end_rfc3339);
    mp_writer_raw(&writer, "}}");
    int status = 0;
    esp_err_t error = writer.valid ? google_post(
        "https://www.googleapis.com/calendar/v3/calendars/primary/events", s_request_body, &status,
        output, size) : ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK && status == 200) {
        strlcpy(output, "Calendar event created.", size);
        return ESP_OK;
    }
    return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
}

static esp_err_t google_post(const char *url, const char *body, int *status, char *output, size_t size)
{
    const char *token;
    esp_err_t error = google_token(&token, output, size);
    if (error != ESP_OK) {
        return error;
    }
    char authorization[1560];
    snprintf(authorization, sizeof(authorization), "Bearer %s", token);
    mp_http_header_t headers[] = {
        {"Authorization", authorization},
        {"Content-Type", "application/json"}
    };
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .headers = headers,
        .header_count = 2,
        .body = body,
        .body_size = strlen(body),
        .response_limit = sizeof(s_response) - 1,
        .timeout_ms = 20000,
        .accepted_content_types = "application/json"
    };
    mp_http_response_t response = {0};
    error = mp_http_collect(&request, s_response, sizeof(s_response), &response);
    *status = response.status;
    if (error == ESP_OK && response.status != 200) {
        google_error(output, size, response.status);
    }
    return error;
}

const mp_email_service_t *mp_email_service(void)
{
    return &s_email;
}

const mp_calendar_service_t *mp_calendar_service(void)
{
    return &s_calendar;
}
