#include "mp_net.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "mp_metrics.h"
#include "sdkconfig.h"

typedef struct {
    char *output;
    size_t size;
    size_t used;
} collect_context_t;

static esp_err_t http_event(esp_http_client_event_t *event);
static bool collect_chunk(const uint8_t *data, size_t size, void *context);
static bool content_type_allowed(const char *actual, const char *accepted);
static bool url_origin(const char *url, char *output, size_t size);
static esp_err_t write_body(esp_http_client_handle_t client, const char *body, size_t size);
static void close_connection(mp_http_session_t *session);
esp_err_t mp_http_session_stream(mp_http_session_t *session, const mp_http_request_t *request,
                                 mp_http_chunk_fn callback, void *context,
                                 mp_http_response_t *response);
esp_err_t mp_http_session_collect(mp_http_session_t *session, const mp_http_request_t *request,
                                  char *output, size_t size, mp_http_response_t *response);
void mp_http_session_cleanup(mp_http_session_t *session);
esp_err_t mp_http_stream(const mp_http_request_t *request, mp_http_chunk_fn callback,
                         void *context, mp_http_response_t *response);
esp_err_t mp_http_collect(const mp_http_request_t *request, char *output, size_t size,
                          mp_http_response_t *response);
bool mp_url_is_https(const char *url);
bool mp_url_is_public_https(const char *url);

static esp_err_t http_event(esp_http_client_event_t *event)
{
    mp_http_session_t *session = event->user_data;
    mp_http_response_t *response = session ? session->active_response : NULL;
    if (event->event_id != HTTP_EVENT_ON_HEADER || !response ||
        !event->header_key || !event->header_value) {
        return ESP_OK;
    }
    if (strcasecmp(event->header_key, "Content-Type") == 0) {
        strlcpy(response->content_type, event->header_value, sizeof(response->content_type));
    } else if (strcasecmp(event->header_key, "Location") == 0) {
        strlcpy(response->location, event->header_value, sizeof(response->location));
    }
    return ESP_OK;
}

static bool collect_chunk(const uint8_t *data, size_t size, void *context)
{
    collect_context_t *collect = context;
    size_t available = collect->size - collect->used - 1;
    size_t copied = size < available ? size : available;
    memcpy(collect->output + collect->used, data, copied);
    collect->used += copied;
    collect->output[collect->used] = 0;
    return copied == size;
}

static bool content_type_allowed(const char *actual, const char *accepted)
{
    if (!accepted) {
        return true;
    }
    while (*accepted) {
        const char *end = strchr(accepted, ',');
        size_t length = end ? (size_t)(end - accepted) : strlen(accepted);
        if (strncasecmp(actual, accepted, length) == 0) {
            return true;
        }
        accepted = end ? end + 1 : accepted + length;
    }
    return false;
}

static bool url_origin(const char *url, char *output, size_t size)
{
    if (!mp_url_is_https(url)) {
        return false;
    }
    const char *end = strchr(url + 8, '/');
    size_t length = end ? (size_t)(end - url) : strlen(url);
    if (length >= size) {
        return false;
    }
    memcpy(output, url, length);
    output[length] = 0;
    return true;
}

static esp_err_t write_body(esp_http_client_handle_t client, const char *body, size_t size)
{
    size_t written = 0;
    while (written < size) {
        int count = esp_http_client_write(client, body + written, size - written);
        if (count <= 0) {
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        written += count;
    }
    return ESP_OK;
}

static void close_connection(mp_http_session_t *session)
{
    if (session->handle) {
        esp_http_client_close(session->handle);
    }
    session->ready = false;
}

esp_err_t mp_http_session_stream(mp_http_session_t *session, const mp_http_request_t *request,
                                 mp_http_chunk_fn callback, void *context,
                                 mp_http_response_t *response)
{
    if (!session || !request || !request->url || !response || !mp_url_is_https(request->url) ||
        request->body_size > INT_MAX || (request->body_size && !request->body && !request->write) ||
        (request->body && request->write)) {
        return ESP_ERR_INVALID_ARG;
    }
    char origin[sizeof(session->origin)];
    if (!url_origin(request->url, origin, sizeof(origin))) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t buffer_size = request->buffer_size ? request->buffer_size : 1024;
    if (session->handle && (strcmp(session->origin, origin) != 0 ||
                           session->buffer_size != buffer_size)) {
        mp_http_session_cleanup(session);
    }
    memset(response, 0, sizeof(*response));
    int64_t started = esp_timer_get_time();
    if (!session->handle) {
        esp_http_client_config_t config = {
            .url = request->url,
            .method = request->method,
            .timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 15000,
            .disable_auto_redirect = true,
            .max_authorization_retries = 0,
            .event_handler = http_event,
            .user_data = session,
            .buffer_size = buffer_size,
            .buffer_size_tx = buffer_size,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
            .keep_alive_idle = 20,
            .keep_alive_interval = 10,
            .keep_alive_count = 3,
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
            .save_client_session = true,
#endif
        };
        session->handle = esp_http_client_init(&config);
        if (!session->handle) {
            return ESP_ERR_NO_MEM;
        }
        strlcpy(session->origin, origin, sizeof(session->origin));
        session->buffer_size = buffer_size;
    }
    session->active_response = response;
    response->reused = session->ready;
    esp_http_client_set_url(session->handle, request->url);
    esp_http_client_set_method(session->handle, request->method);
    esp_http_client_set_timeout_ms(session->handle,
                                   request->timeout_ms > 0 ? request->timeout_ms : 15000);
    for (size_t index = 0; index < request->header_count; index++) {
        esp_http_client_set_header(session->handle, request->headers[index].name,
                                   request->headers[index].value);
    }
    esp_err_t error;
    if (session->ready) {
        error = esp_http_client_prepare(session->handle);
        if (error == ESP_OK) {
            error = esp_http_client_request_send(session->handle, (int)request->body_size);
        }
    } else {
        error = esp_http_client_open(session->handle, (int)request->body_size);
    }
    response->connect_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    if (error == ESP_OK && request->body_size) {
        error = request->write ? request->write(session->handle, request->write_context) :
                write_body(session->handle, request->body, request->body_size);
    }
    if (error == ESP_OK && esp_http_client_fetch_headers(session->handle) < 0) {
        error = ESP_ERR_HTTP_FETCH_HEADER;
    }
    response->first_byte_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    if (error == ESP_OK) {
        response->status = esp_http_client_get_status_code(session->handle);
        if (!content_type_allowed(response->content_type, request->accepted_content_types)) {
            error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    bool complete = false;
    if (error == ESP_OK) {
        uint8_t buffer[1024];
        size_t limit = request->response_limit ? request->response_limit : SIZE_MAX;
        while (response->bytes < limit) {
            size_t available = limit - response->bytes;
            int count = esp_http_client_read(session->handle, (char *)buffer,
                                              available < sizeof(buffer) ? available : sizeof(buffer));
            if (count < 0) {
                error = ESP_FAIL;
                break;
            }
            if (count == 0) {
                complete = esp_http_client_is_complete_data_received(session->handle);
                break;
            }
            response->bytes += count;
            if (callback && !callback(buffer, count, context)) {
                response->truncated = true;
                break;
            }
        }
        if (response->bytes == limit && !esp_http_client_is_complete_data_received(session->handle)) {
            response->truncated = true;
        } else if (!response->truncated) {
            complete = esp_http_client_is_complete_data_received(session->handle);
        }
    }
    session->ready = error == ESP_OK && complete &&
                     esp_http_client_is_persistent_connection(session->handle);
    if (!session->ready) {
        close_connection(session);
    }
    session->active_response = NULL;
    response->total_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    mp_metrics_http(response->status, response->bytes, response->connect_ms,
                    response->first_byte_ms, response->total_ms, response->reused);
    return error;
}

esp_err_t mp_http_session_collect(mp_http_session_t *session, const mp_http_request_t *request,
                                  char *output, size_t size, mp_http_response_t *response)
{
    if (!output || size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    collect_context_t context = {.output = output, .size = size};
    output[0] = 0;
    esp_err_t error = mp_http_session_stream(session, request, collect_chunk, &context, response);
    if (context.used == size - 1) {
        response->truncated = true;
    }
    return error;
}

void mp_http_session_cleanup(mp_http_session_t *session)
{
    if (!session) {
        return;
    }
    if (session->handle) {
        esp_http_client_cleanup(session->handle);
    }
    memset(session, 0, sizeof(*session));
}

esp_err_t mp_http_stream(const mp_http_request_t *request, mp_http_chunk_fn callback,
                         void *context, mp_http_response_t *response)
{
    mp_http_session_t session = {0};
    esp_err_t error = mp_http_session_stream(&session, request, callback, context, response);
    mp_http_session_cleanup(&session);
    return error;
}

esp_err_t mp_http_collect(const mp_http_request_t *request, char *output, size_t size,
                          mp_http_response_t *response)
{
    mp_http_session_t session = {0};
    esp_err_t error = mp_http_session_collect(&session, request, output, size, response);
    mp_http_session_cleanup(&session);
    return error;
}

bool mp_url_is_https(const char *url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

bool mp_url_is_public_https(const char *url)
{
    if (!mp_url_is_https(url)) {
        return false;
    }
    const char *host = url + 8;
    const char *end = strpbrk(host, "/:?#");
    size_t length = end ? (size_t)(end - host) : strlen(host);
    if (!length || length >= 128 || memchr(host, '@', length) || host[0] == '[') {
        return false;
    }
    char name[128];
    memcpy(name, host, length);
    name[length] = 0;
    if (strcasecmp(name, "localhost") == 0 ||
        (length > 6 && strcasecmp(name + length - 6, ".local") == 0)) {
        return false;
    }
    bool numeric = true;
    for (size_t index = 0; index < length; index++) {
        if (!isdigit((unsigned char)name[index]) && name[index] != '.') {
            numeric = false;
            break;
        }
    }
    return !numeric;
}
