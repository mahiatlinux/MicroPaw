#include "mp_tools.h"

#include <string.h>

#include "mp_html.h"
#include "mp_net.h"
#include "mp_search.h"
#include "sdkconfig.h"

static bool page_chunk(const uint8_t *data, size_t size, void *context);
static bool redirect_status(int status);
static esp_err_t fetch_once(const char *url, mp_html_parser_t *parser, mp_http_response_t *response);
esp_err_t mp_page_fetch(const char *url, char *output, size_t size);

static bool page_chunk(const uint8_t *data, size_t size, void *context)
{
    mp_html_parser_t *parser = context;
    for (size_t index = 0; index < size; index++) {
        if (!mp_html_parser_push(parser, (char)data[index])) {
            return false;
        }
    }
    return true;
}

static bool redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static esp_err_t fetch_once(const char *url, mp_html_parser_t *parser, mp_http_response_t *response)
{
    mp_http_header_t headers[] = {
        {"Accept", "text/html,text/plain,application/xhtml+xml"},
        {"User-Agent", "MicroPaw/0.1 ESP32"}
    };
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
        .response_limit = CONFIG_MICROPAW_PAGE_DOWNLOAD_LIMIT,
        .timeout_ms = 20000,
        .accepted_content_types = "text/html,text/plain,application/xhtml+xml"
    };
    return mp_http_stream(&request, page_chunk, parser, response);
}

esp_err_t mp_page_fetch(const char *url, char *output, size_t size)
{
    if (!mp_search_url_allowed(url) || !output || size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_html_parser_t parser;
    mp_html_parser_init(&parser, output, size);
    mp_http_response_t response;
    esp_err_t error = fetch_once(url, &parser, &response);
    if (error != ESP_OK) {
        return error;
    }
    if (redirect_status(response.status)) {
        if (!mp_url_is_public_https(response.location)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        mp_html_parser_init(&parser, output, size);
        error = fetch_once(response.location, &parser, &response);
        if (error != ESP_OK) {
            return error;
        }
    }
    mp_html_parser_finish(&parser);
    return response.status >= 200 && response.status < 300 && output[0] ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
