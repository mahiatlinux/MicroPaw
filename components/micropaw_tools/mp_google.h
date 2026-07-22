#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "mp_net.h"

esp_err_t mp_google_request(esp_http_client_method_t method, const char *url,
                            const char *body, size_t body_size, mp_http_write_fn write,
                            void *write_context, int timeout_ms, const char *accepted_content_types,
                            mp_http_response_t *response, char *output, size_t size);
const char *mp_google_response(void);
