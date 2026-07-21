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

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    const mp_http_header_t *headers;
    size_t header_count;
    const char *body;
    size_t body_size;
    size_t response_limit;
    int timeout_ms;
    const char *accepted_content_types;
} mp_http_request_t;

typedef struct {
    int status;
    size_t bytes;
    bool truncated;
    char content_type[64];
    char location[512];
} mp_http_response_t;

esp_err_t mp_net_init(void);
esp_err_t mp_http_stream(const mp_http_request_t *request, mp_http_chunk_fn callback,
                         void *context, mp_http_response_t *response);
esp_err_t mp_http_collect(const mp_http_request_t *request, char *output, size_t size,
                          mp_http_response_t *response);
bool mp_url_is_https(const char *url);
bool mp_url_is_public_https(const char *url);
