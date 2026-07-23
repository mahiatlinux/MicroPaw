#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"

typedef struct {
    const char *name;
    const char *value;
} mp_http_header_t;

typedef bool (*mp_http_chunk_fn)(const uint8_t *data, size_t size, void *context);
typedef esp_err_t (*mp_http_write_fn)(esp_http_client_handle_t client, void *context);

typedef struct {
    esp_http_client_handle_t handle;
    char origin[160];
    size_t buffer_size;
    void *active_response;
    bool ready;
} mp_http_session_t;

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    const mp_http_header_t *headers;
    size_t header_count;
    const char *body;
    size_t body_size;
    mp_http_write_fn write;
    void *write_context;
    size_t response_limit;
    int timeout_ms;
    size_t buffer_size;
    const char *accepted_content_types;
} mp_http_request_t;

typedef struct {
    int status;
    size_t bytes;
    bool truncated;
    bool reused;
    uint32_t connect_ms;
    uint32_t first_byte_ms;
    uint32_t total_ms;
    char content_type[160];
    char location[512];
} mp_http_response_t;

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
