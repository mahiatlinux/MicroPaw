#include "mp_tools.h"

#include <ctype.h>
#include <string.h>

#include "mp_html.h"
#include "mp_net.h"
#include "mp_search.h"
#include "sdkconfig.h"

typedef struct {
    bool in_tag;
    char tag[32];
    size_t tag_length;
    bool tag_done;
    bool suppress;
    mp_html_text_t text;
} page_parser_t;

static void page_tag(page_parser_t *parser);
static bool page_chunk(const uint8_t *data, size_t size, void *context);
static bool redirect_status(int status);
static esp_err_t fetch_once(const char *url, page_parser_t *parser, mp_http_response_t *response);
esp_err_t mp_page_fetch(const char *url, char *output, size_t size);

static void page_tag(page_parser_t *parser)
{
    parser->tag[parser->tag_length] = 0;
    if (strcmp(parser->tag, "script") == 0 || strcmp(parser->tag, "style") == 0 ||
        strcmp(parser->tag, "noscript") == 0) {
        parser->suppress = true;
    } else if (strcmp(parser->tag, "/script") == 0 || strcmp(parser->tag, "/style") == 0 ||
               strcmp(parser->tag, "/noscript") == 0) {
        parser->suppress = false;
    } else if (!parser->suppress && (parser->tag[0] == '/' || strcmp(parser->tag, "br") == 0 ||
               strcmp(parser->tag, "p") == 0 || strcmp(parser->tag, "li") == 0)) {
        mp_html_text_push(&parser->text, ' ');
    }
    parser->tag_length = 0;
}

static bool page_chunk(const uint8_t *data, size_t size, void *context)
{
    page_parser_t *parser = context;
    for (size_t index = 0; index < size; index++) {
        char value = (char)data[index];
        if (parser->in_tag) {
            if (value == '>') {
                parser->in_tag = false;
                page_tag(parser);
            } else if (isspace((unsigned char)value)) {
                parser->tag_done = true;
            } else if (!parser->tag_done && parser->tag_length + 1 < sizeof(parser->tag) &&
                       (isalpha((unsigned char)value) || (value == '/' && parser->tag_length == 0))) {
                parser->tag[parser->tag_length++] = (char)tolower((unsigned char)value);
            }
        } else if (value == '<') {
            parser->in_tag = true;
            parser->tag_length = 0;
            parser->tag_done = false;
        } else if (!parser->suppress) {
            mp_html_text_push(&parser->text, value);
        }
        if (parser->text.length + 1 >= parser->text.size) {
            return false;
        }
    }
    return true;
}

static bool redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static esp_err_t fetch_once(const char *url, page_parser_t *parser, mp_http_response_t *response)
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
    page_parser_t parser = {0};
    mp_html_text_init(&parser.text, output, size);
    mp_http_response_t response;
    esp_err_t error = fetch_once(url, &parser, &response);
    if (error != ESP_OK) {
        return error;
    }
    if (redirect_status(response.status)) {
        if (!mp_url_is_public_https(response.location)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        memset(&parser, 0, sizeof(parser));
        mp_html_text_init(&parser.text, output, size);
        error = fetch_once(response.location, &parser, &response);
        if (error != ESP_OK) {
            return error;
        }
    }
    mp_html_text_finish(&parser.text);
    return response.status >= 200 && response.status < 300 && output[0] ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
