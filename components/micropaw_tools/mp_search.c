#include "mp_search.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "mp_html.h"
#include "mp_json.h"
#include "mp_net.h"
#include "sdkconfig.h"

#include "mp_feed.h"

extern const mp_search_provider_t mp_wikipedia_provider;

typedef enum {
    CAPTURE_NONE,
    CAPTURE_TITLE,
    CAPTURE_SNIPPET
} capture_t;

typedef struct {
    mp_search_result_t *results;
    size_t count;
    bool in_tag;
    char tag[768];
    size_t tag_length;
    capture_t capture;
    mp_search_result_t current;
    mp_html_text_t title;
    mp_html_text_t snippet;
} ddg_parser_t;

EXT_RAM_BSS_ATTR static char s_allowed[MP_SEARCH_MAX_RESULTS][MP_SEARCH_URL_LEN];
EXT_RAM_BSS_ATTR static mp_search_result_t s_results[MP_SEARCH_MAX_RESULTS];

static int hex_value(char value);
static void decode_url(const char *source, size_t length, char *output, size_t size);
static void extract_url(const char *tag, char *output, size_t size);
static void process_tag(ddg_parser_t *parser);
static bool ddg_chunk(const uint8_t *data, size_t size, void *context);
static esp_err_t ddg_request(const char *url, const char *body, size_t body_size,
                             mp_search_result_t *results, size_t *count);
static esp_err_t ddg_search(const char *query, mp_search_result_t *results, size_t *count);
const mp_search_provider_t *mp_search_provider(const char *name);
esp_err_t mp_search_run(const char *provider, const char *query, char *output, size_t size);
bool mp_search_url_allowed(const char *url);

static const mp_search_provider_t s_duckduckgo_provider = {"duckduckgo", ddg_search};

static const mp_search_provider_t *const s_providers[] = {
    &s_duckduckgo_provider,
#if CONFIG_MICROPAW_WIKIPEDIA
    &mp_wikipedia_provider,
#endif
#if CONFIG_MICROPAW_ARXIV
    &mp_arxiv_provider,
#endif
};

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static void decode_url(const char *source, size_t length, char *output, size_t size)
{
    size_t used = 0;
    for (size_t index = 0; index < length && used + 1 < size; index++) {
        if (source[index] == '%' && index + 2 < length) {
            int high = hex_value(source[index + 1]);
            int low = hex_value(source[index + 2]);
            if (high >= 0 && low >= 0) {
                output[used++] = (char)((high << 4) | low);
                index += 2;
                continue;
            }
        }
        output[used++] = source[index] == '+' ? ' ' : source[index];
    }
    output[used] = 0;
}

static void extract_url(const char *tag, char *output, size_t size)
{
    output[0] = 0;
    const char *href = strstr(tag, "href=");
    if (!href) {
        return;
    }
    href += 5;
    char quote = *href;
    if (quote == '\'' || quote == '"') {
        href++;
    } else {
        quote = ' ';
    }
    const char *end = href;
    while (*end && *end != quote && *end != '>') {
        end++;
    }
    const char *encoded = strstr(href, "uddg=");
    if (encoded && encoded < end) {
        encoded += 5;
        const char *encoded_end = encoded;
        while (encoded_end < end && *encoded_end != '&') {
            encoded_end++;
        }
        decode_url(encoded, encoded_end - encoded, output, size);
    } else if ((size_t)(end - href) < size) {
        memcpy(output, href, end - href);
        output[end - href] = 0;
    }
}

static void process_tag(ddg_parser_t *parser)
{
    parser->tag[parser->tag_length] = 0;
    if (parser->tag[0] == 'a' && isspace((unsigned char)parser->tag[1]) &&
        (strstr(parser->tag, "result-link") || strstr(parser->tag, "result__a"))) {
        memset(&parser->current, 0, sizeof(parser->current));
        extract_url(parser->tag, parser->current.url, sizeof(parser->current.url));
        mp_html_text_init(&parser->title, parser->current.title, sizeof(parser->current.title));
        parser->capture = CAPTURE_TITLE;
    } else if (strcmp(parser->tag, "/a") == 0 && parser->capture == CAPTURE_TITLE) {
        mp_html_text_finish(&parser->title);
        parser->capture = CAPTURE_NONE;
    } else if ((parser->tag[0] == 't' && parser->tag[1] == 'd' &&
                strstr(parser->tag, "result-snippet")) ||
               (parser->tag[0] == 'a' && isspace((unsigned char)parser->tag[1]) &&
                strstr(parser->tag, "result__snippet"))) {
        mp_html_text_init(&parser->snippet, parser->current.snippet, sizeof(parser->current.snippet));
        parser->capture = CAPTURE_SNIPPET;
    } else if ((strcmp(parser->tag, "/td") == 0 || strcmp(parser->tag, "/a") == 0) &&
               parser->capture == CAPTURE_SNIPPET) {
        mp_html_text_finish(&parser->snippet);
        parser->capture = CAPTURE_NONE;
        if (parser->current.title[0] && parser->current.url[0] && parser->count < MP_SEARCH_MAX_RESULTS) {
            parser->results[parser->count++] = parser->current;
        }
    }
    parser->tag_length = 0;
}

static bool ddg_chunk(const uint8_t *data, size_t size, void *context)
{
    ddg_parser_t *parser = context;
    for (size_t index = 0; index < size; index++) {
        char value = (char)data[index];
        if (parser->in_tag) {
            if (value == '>') {
                parser->in_tag = false;
                process_tag(parser);
            } else if (parser->tag_length + 1 < sizeof(parser->tag)) {
                parser->tag[parser->tag_length++] = value;
            }
        } else if (value == '<') {
            parser->in_tag = true;
            parser->tag_length = 0;
        } else if (parser->capture == CAPTURE_TITLE) {
            mp_html_text_push(&parser->title, value);
        } else if (parser->capture == CAPTURE_SNIPPET) {
            mp_html_text_push(&parser->snippet, value);
        }
        if (parser->count == MP_SEARCH_MAX_RESULTS) {
            return false;
        }
    }
    return true;
}

static esp_err_t ddg_request(const char *url, const char *body, size_t body_size,
                             mp_search_result_t *results, size_t *count)
{
    mp_http_header_t headers[] = {
        {"Accept", "text/html"},
        {"User-Agent", "MicroPaw/0.1 ESP32"},
        {"Content-Type", "application/x-www-form-urlencoded"}
    };
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
        .body = body,
        .body_size = body_size,
        .response_limit = CONFIG_MICROPAW_SEARCH_DOWNLOAD_LIMIT,
        .timeout_ms = 20000,
        .accepted_content_types = "text/html,application/xhtml+xml"
    };
    ddg_parser_t parser = {.results = results};
    mp_http_response_t response;
    esp_err_t error = mp_http_stream(&request, ddg_chunk, &parser, &response);
    if (error != ESP_OK) {
        return error;
    }
    if (response.status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *count = parser.count;
    return parser.count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t ddg_search(const char *query, mp_search_result_t *results, size_t *count)
{
    char encoded[768];
    char body[770];
    mp_url_encode(query, encoded, sizeof(encoded));
    int body_length = snprintf(body, sizeof(body), "q=%s", encoded);
    if (body_length < 0 || (size_t)body_length >= sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = ddg_request("https://lite.duckduckgo.com/lite/", body,
                                  (size_t)body_length, results, count);
    if (error == ESP_ERR_INVALID_RESPONSE || error == ESP_ERR_NOT_FOUND) {
        error = ddg_request("https://html.duckduckgo.com/html/", body,
                            (size_t)body_length, results, count);
    }
    return error;
}

const mp_search_provider_t *mp_search_provider(const char *name)
{
    for (size_t index = 0; index < sizeof(s_providers) / sizeof(s_providers[0]); index++) {
        if (strcmp(name, s_providers[index]->name) == 0) {
            return s_providers[index];
        }
    }
    return NULL;
}

esp_err_t mp_search_run(const char *provider, const char *query, char *output, size_t size)
{
    const mp_search_provider_t *selected = mp_search_provider(provider);
    if (!selected || !query || !query[0] || !output || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_results, 0, sizeof(s_results));
    size_t count = 0;
    esp_err_t error = selected->search(query, s_results, &count);
    if (error != ESP_OK) {
        return error;
    }
    mp_search_allow_results(s_results, count);
    mp_search_format_results(s_results, count, output, size);
    return ESP_OK;
}

bool mp_search_url_allowed(const char *url)
{
    if (!mp_url_is_public_https(url)) {
        return false;
    }
    for (size_t index = 0; index < MP_SEARCH_MAX_RESULTS; index++) {
        if (s_allowed[index][0] && strcmp(url, s_allowed[index]) == 0) {
            return true;
        }
    }
    return false;
}

void mp_search_allow_results(const mp_search_result_t *results, size_t count)
{
    memset(s_allowed, 0, sizeof(s_allowed));
    for (size_t index = 0; index < count && index < MP_SEARCH_MAX_RESULTS; index++) {
        strlcpy(s_allowed[index], results[index].url, sizeof(s_allowed[index]));
    }
}

void mp_search_format_results(const mp_search_result_t *results, size_t count,
                              char *output, size_t size)
{
    output[0] = 0;
    for (size_t index = 0; index < count; index++) {
        size_t used = strnlen(output, size);
        if (used < size) {
            snprintf(output + used, size - used, "[%u] %s\nURL: %s\n%s\n",
                     (unsigned)(index + 1), results[index].title, results[index].url,
                     results[index].snippet);
        }
    }
}
