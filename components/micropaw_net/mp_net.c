#include "mp_net.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    mp_http_response_t *response;
} event_context_t;

typedef struct {
    char *output;
    size_t size;
    size_t used;
} collect_context_t;

static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;

static esp_err_t http_event(esp_http_client_event_t *event);
static bool collect_chunk(const uint8_t *data, size_t size, void *context);
static bool content_type_allowed(const char *actual, const char *accepted);
static esp_err_t write_body(esp_http_client_handle_t client, const char *body, size_t size);
esp_err_t mp_net_init(void);
esp_err_t mp_http_stream(const mp_http_request_t *request, mp_http_chunk_fn callback,
                         void *context, mp_http_response_t *response);
esp_err_t mp_http_collect(const mp_http_request_t *request, char *output, size_t size,
                          mp_http_response_t *response);
bool mp_url_is_https(const char *url);
bool mp_url_is_public_https(const char *url);

static esp_err_t http_event(esp_http_client_event_t *event)
{
    event_context_t *context = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_HEADER || !context || !context->response ||
        !event->header_key || !event->header_value) {
        return ESP_OK;
    }
    if (strcasecmp(event->header_key, "Content-Type") == 0) {
        strlcpy(context->response->content_type, event->header_value,
                sizeof(context->response->content_type));
    } else if (strcasecmp(event->header_key, "Location") == 0) {
        strlcpy(context->response->location, event->header_value,
                sizeof(context->response->location));
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

esp_err_t mp_net_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mp_http_stream(const mp_http_request_t *request, mp_http_chunk_fn callback,
                         void *context, mp_http_response_t *response)
{
    if (!request || !request->url || !response || !mp_url_is_https(request->url) || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(response, 0, sizeof(*response));
    event_context_t event_context = {.response = response};
    esp_http_client_config_t config = {
        .url = request->url,
        .method = request->method,
        .timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 15000,
        .disable_auto_redirect = true,
        .max_authorization_retries = 0,
        .event_handler = http_event,
        .user_data = &event_context,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0; index < request->header_count; index++) {
        esp_http_client_set_header(client, request->headers[index].name, request->headers[index].value);
    }
    esp_err_t error = esp_http_client_open(client, request->body_size);
    if (error == ESP_OK && request->body_size) {
        error = write_body(client, request->body, request->body_size);
    }
    if (error == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        error = ESP_ERR_HTTP_FETCH_HEADER;
    }
    if (error == ESP_OK) {
        response->status = esp_http_client_get_status_code(client);
        if (!content_type_allowed(response->content_type, request->accepted_content_types)) {
            error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (error == ESP_OK) {
        uint8_t buffer[1024];
        size_t limit = request->response_limit ? request->response_limit : SIZE_MAX;
        while (response->bytes < limit) {
            size_t available = limit - response->bytes;
            int count = esp_http_client_read(client, (char *)buffer,
                                              available < sizeof(buffer) ? available : sizeof(buffer));
            if (count < 0) {
                error = ESP_FAIL;
                break;
            }
            if (count == 0) {
                break;
            }
            response->bytes += count;
            if (callback && !callback(buffer, count, context)) {
                response->truncated = true;
                break;
            }
        }
        if (response->bytes == limit) {
            response->truncated = true;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t mp_http_collect(const mp_http_request_t *request, char *output, size_t size,
                          mp_http_response_t *response)
{
    if (!output || size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    collect_context_t context = {.output = output, .size = size, .used = 0};
    output[0] = 0;
    esp_err_t error = mp_http_stream(request, collect_chunk, &context, response);
    if (context.used == size - 1) {
        response->truncated = true;
    }
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
