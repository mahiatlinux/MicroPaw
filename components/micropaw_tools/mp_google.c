#include "mp_services.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "mp_config.h"
#include "mp_json.h"
#include "mp_net.h"

typedef struct {
    const char *header;
    size_t header_size;
    const char *body;
} email_write_context_t;

typedef struct {
    esp_http_client_handle_t client;
    size_t output_size;
    uint8_t tail[3];
    size_t tail_size;
} base64_stream_t;

EXT_RAM_BSS_ATTR static char s_token[1536];
EXT_RAM_BSS_ATTR static char s_refresh_body[1024];
EXT_RAM_BSS_ATTR static char s_email_header[1024];
EXT_RAM_BSS_ATTR static char s_encode_buffer[2048];
EXT_RAM_BSS_ATTR static char s_calendar_body[2048];
EXT_RAM_BSS_ATTR static char s_response[2048];
static time_t s_token_expiry;

static void google_error(char *output, size_t size, int status);
static esp_err_t google_token(const char **token, char *output, size_t size);
static esp_err_t google_email(const mp_email_t *email, char *output, size_t size);
static esp_err_t google_calendar(const mp_calendar_event_t *event, char *output, size_t size);
static esp_err_t google_post(const char *url, const char *body, size_t body_size,
                             mp_http_write_fn write, void *write_context, int timeout_ms,
                             int *status, char *output, size_t size);
static esp_err_t write_all(esp_http_client_handle_t client, const char *data, size_t size);
static esp_err_t base64_flush(base64_stream_t *stream);
static esp_err_t base64_emit(base64_stream_t *stream, const uint8_t *input, size_t size);
static esp_err_t base64_feed(base64_stream_t *stream, const uint8_t *input, size_t size);
static esp_err_t base64_finish(base64_stream_t *stream);
static esp_err_t email_write(esp_http_client_handle_t client, void *context);
static size_t base64url_size(size_t size);
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
    if (!email || !email->to || !email->subject || !email->body ||
        strchr(email->to, '\r') || strchr(email->to, '\n') ||
        strchr(email->subject, '\r') || strchr(email->subject, '\n')) {
        return ESP_ERR_INVALID_ARG;
    }
    int length = snprintf(s_email_header, sizeof(s_email_header),
                          "To: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n",
                          email->to, email->subject);
    if (length < 0 || (size_t)length >= sizeof(s_email_header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t raw_size = (size_t)length + strlen(email->body);
    if (raw_size < (size_t)length || raw_size > (SIZE_MAX - 10) / 4 * 3) {
        return ESP_ERR_INVALID_SIZE;
    }
    email_write_context_t context = {
        .header = s_email_header,
        .header_size = (size_t)length,
        .body = email->body
    };
    int status = 0;
    esp_err_t error = google_post(
        "https://gmail.googleapis.com/gmail/v1/users/me/messages/send", NULL,
        base64url_size(raw_size) + 10, email_write, &context, 60000, &status, output, size);
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
    mp_writer_init(&writer, s_calendar_body, sizeof(s_calendar_body));
    mp_writer_raw(&writer, "{\"summary\":");
    mp_writer_string(&writer, event->summary);
    mp_writer_raw(&writer, ",\"start\":{\"dateTime\":");
    mp_writer_string(&writer, event->start_rfc3339);
    mp_writer_raw(&writer, "},\"end\":{\"dateTime\":");
    mp_writer_string(&writer, event->end_rfc3339);
    mp_writer_raw(&writer, "}}");
    int status = 0;
    esp_err_t error = writer.valid ? google_post(
        "https://www.googleapis.com/calendar/v3/calendars/primary/events", s_calendar_body,
        writer.length, NULL, NULL, 20000, &status, output, size) : ESP_ERR_INVALID_SIZE;
    if (error == ESP_OK && status == 200) {
        strlcpy(output, "Calendar event created.", size);
        return ESP_OK;
    }
    return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
}

static esp_err_t google_post(const char *url, const char *body, size_t body_size,
                             mp_http_write_fn write, void *write_context, int timeout_ms,
                             int *status, char *output, size_t size)
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
        .body_size = body_size,
        .write = write,
        .write_context = write_context,
        .response_limit = sizeof(s_response) - 1,
        .timeout_ms = timeout_ms,
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

static esp_err_t write_all(esp_http_client_handle_t client, const char *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        int count = esp_http_client_write(client, data + written, size - written);
        if (count <= 0) {
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        written += count;
    }
    return ESP_OK;
}

static esp_err_t base64_flush(base64_stream_t *stream)
{
    esp_err_t error = write_all(stream->client, s_encode_buffer, stream->output_size);
    stream->output_size = 0;
    return error;
}

static esp_err_t base64_emit(base64_stream_t *stream, const uint8_t *input, size_t size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (stream->output_size + 4 > sizeof(s_encode_buffer)) {
        esp_err_t error = base64_flush(stream);
        if (error != ESP_OK) {
            return error;
        }
    }
    uint32_t value = (uint32_t)input[0] << 16;
    if (size > 1) {
        value |= (uint32_t)input[1] << 8;
    }
    if (size > 2) {
        value |= input[2];
    }
    s_encode_buffer[stream->output_size++] = alphabet[(value >> 18) & 63];
    s_encode_buffer[stream->output_size++] = alphabet[(value >> 12) & 63];
    if (size > 1) {
        s_encode_buffer[stream->output_size++] = alphabet[(value >> 6) & 63];
    }
    if (size > 2) {
        s_encode_buffer[stream->output_size++] = alphabet[value & 63];
    }
    return ESP_OK;
}

static esp_err_t base64_feed(base64_stream_t *stream, const uint8_t *input, size_t size)
{
    if (stream->tail_size) {
        while (stream->tail_size < 3 && size) {
            stream->tail[stream->tail_size++] = *input++;
            size--;
        }
        if (stream->tail_size == 3) {
            esp_err_t error = base64_emit(stream, stream->tail, 3);
            if (error != ESP_OK) {
                return error;
            }
            stream->tail_size = 0;
        }
    }
    while (size >= 3) {
        esp_err_t error = base64_emit(stream, input, 3);
        if (error != ESP_OK) {
            return error;
        }
        input += 3;
        size -= 3;
    }
    while (size) {
        stream->tail[stream->tail_size++] = *input++;
        size--;
    }
    return ESP_OK;
}

static esp_err_t base64_finish(base64_stream_t *stream)
{
    if (stream->tail_size) {
        esp_err_t error = base64_emit(stream, stream->tail, stream->tail_size);
        if (error != ESP_OK) {
            return error;
        }
    }
    return base64_flush(stream);
}

static esp_err_t email_write(esp_http_client_handle_t client, void *context)
{
    email_write_context_t *email = context;
    esp_err_t error = write_all(client, "{\"raw\":\"", 8);
    base64_stream_t stream = {.client = client};
    if (error == ESP_OK) {
        error = base64_feed(&stream, (const uint8_t *)email->header, email->header_size);
    }
    if (error == ESP_OK) {
        error = base64_feed(&stream, (const uint8_t *)email->body, strlen(email->body));
    }
    if (error == ESP_OK) {
        error = base64_finish(&stream);
    }
    if (error == ESP_OK) {
        error = write_all(client, "\"}", 2);
    }
    return error;
}

static size_t base64url_size(size_t size)
{
    size_t encoded = size / 3 * 4;
    size_t remainder = size % 3;
    return encoded + (remainder ? remainder + 1 : 0);
}

const mp_email_service_t *mp_email_service(void)
{
    return &s_email;
}

const mp_calendar_service_t *mp_calendar_service(void)
{
    return &s_calendar;
}
