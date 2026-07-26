#include "mp_search.h"

#include <stdio.h>
#include <string.h>

#include "mp_html.h"
#include "mp_json.h"
#include "mp_net.h"
#include "sdkconfig.h"

typedef struct {
    mp_search_result_t *results;
    size_t count;
    char object[3072];
    size_t object_length;
    size_t pattern;
    int depth;
    bool pages;
    bool waiting_array;
    bool capture;
    bool quoted;
    bool escaped;
    bool overflow;
} wiki_parser_t;

static void strip_excerpt(const char *source, char *output, size_t size);
static void wiki_object(wiki_parser_t *parser);
static bool wiki_chunk(const uint8_t *data, size_t size, void *context);
static esp_err_t wikipedia_search(const char *query, mp_search_result_t *results, size_t *count,
                                  char *error_text, size_t error_size);

const mp_search_provider_t mp_wikipedia_provider = {"wikipedia", wikipedia_search};

static void strip_excerpt(const char *source, char *output, size_t size)
{
    bool tag = false;
    mp_html_text_t text;
    mp_html_text_init(&text, output, size);
    while (*source) {
        if (*source == '<') {
            tag = true;
        } else if (*source == '>') {
            tag = false;
        } else if (!tag) {
            mp_html_text_push(&text, *source);
        }
        source++;
    }
    mp_html_text_finish(&text);
}

static void wiki_object(wiki_parser_t *parser)
{
    if (parser->overflow || parser->count == MP_SEARCH_MAX_RESULTS) {
        return;
    }
    parser->object[parser->object_length] = 0;
    mp_search_result_t *result = &parser->results[parser->count];
    char excerpt[1024] = {0};
    if (!mp_json_get_string(parser->object, parser->object_length, "title",
                            result->title, sizeof(result->title))) {
        return;
    }
    mp_json_get_string(parser->object, parser->object_length, "excerpt", excerpt, sizeof(excerpt));
    strip_excerpt(excerpt, result->snippet, sizeof(result->snippet));
    char encoded[480];
    mp_url_encode(result->title, encoded, sizeof(encoded));
    snprintf(result->url, sizeof(result->url), "https://en.wikipedia.org/wiki/%s", encoded);
    parser->count++;
}

static bool wiki_chunk(const uint8_t *data, size_t size, void *context)
{
    static const char pattern[] = "\"pages\"";
    wiki_parser_t *parser = context;
    for (size_t index = 0; index < size; index++) {
        char value = (char)data[index];
        if (!parser->pages) {
            if (parser->waiting_array) {
                if (value == '[') {
                    parser->pages = true;
                }
            } else if (value == pattern[parser->pattern]) {
                if (++parser->pattern == sizeof(pattern) - 1) {
                    parser->waiting_array = true;
                }
            } else {
                parser->pattern = value == pattern[0] ? 1 : 0;
            }
            continue;
        }
        if (!parser->capture) {
            if (value == '{') {
                parser->capture = true;
                parser->depth = 1;
                parser->object_length = 0;
                parser->overflow = false;
                parser->quoted = false;
                parser->escaped = false;
                parser->object[parser->object_length++] = value;
            }
            continue;
        }
        if (parser->object_length + 1 < sizeof(parser->object)) {
            parser->object[parser->object_length++] = value;
        } else {
            parser->overflow = true;
        }
        if (parser->quoted) {
            if (parser->escaped) {
                parser->escaped = false;
            } else if (value == '\\') {
                parser->escaped = true;
            } else if (value == '"') {
                parser->quoted = false;
            }
        } else if (value == '"') {
            parser->quoted = true;
        } else if (value == '{' || value == '[') {
            parser->depth++;
        } else if (value == '}' || value == ']') {
            parser->depth--;
            if (parser->depth == 0) {
                parser->capture = false;
                wiki_object(parser);
                if (parser->count == MP_SEARCH_MAX_RESULTS) {
                    return false;
                }
            }
        }
    }
    return true;
}

static esp_err_t wikipedia_search(const char *query, mp_search_result_t *results, size_t *count,
                                  char *error_text, size_t error_size)
{
    (void)error_text;
    (void)error_size;
    char encoded[768];
    char url[896];
    mp_url_encode(query, encoded, sizeof(encoded));
    snprintf(url, sizeof(url), "https://en.wikipedia.org/w/rest.php/v1/search/page?q=%s&limit=5", encoded);
    mp_http_header_t headers[] = {{"User-Agent", "MicroPaw/0.1 ESP32 personal assistant"}};
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .headers = headers,
        .header_count = 1,
        .response_limit = CONFIG_MICROPAW_SEARCH_DOWNLOAD_LIMIT,
        .timeout_ms = 20000,
        .accepted_content_types = "application/json"
    };
    wiki_parser_t parser = {.results = results};
    mp_http_response_t response;
    esp_err_t error = mp_http_stream(&request, wiki_chunk, &parser, &response);
    if (error != ESP_OK || response.status != 200) {
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    *count = parser.count;
    return parser.count ? ESP_OK : ESP_ERR_NOT_FOUND;
}
