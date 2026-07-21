#include "mp_feed.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "mp_html.h"
#include "mp_json.h"
#include "mp_net.h"
#include "sdkconfig.h"

typedef enum {
    FEED_NONE,
    FEED_TITLE,
    FEED_URL,
    FEED_SNIPPET
} feed_field_t;

typedef struct {
    mp_search_result_t *results;
    size_t count;
    mp_search_result_t current;
    bool in_tag;
    bool in_entry;
    bool cdata;
    unsigned cdata_close;
    char tag[512];
    size_t tag_length;
    feed_field_t field;
    mp_html_text_t text;
} feed_parser_t;

static bool tag_name(const char *tag, const char *name, bool closing);
static void extract_href(const char *tag, char *output, size_t size);
static void feed_start_text(feed_parser_t *parser, feed_field_t field, char *output, size_t size);
static void feed_tag(feed_parser_t *parser);
static void feed_cdata(feed_parser_t *parser, char value);
static bool feed_chunk(const uint8_t *data, size_t size, void *context);
static esp_err_t feed_request(const char *url, mp_search_result_t *results, size_t *count);
static esp_err_t arxiv_search(const char *query, mp_search_result_t *results, size_t *count);
esp_err_t mp_rss_read(const char *url, char *output, size_t size);

const mp_search_provider_t mp_arxiv_provider = {"arxiv", arxiv_search};
EXT_RAM_BSS_ATTR static mp_search_result_t s_feed_results[MP_SEARCH_MAX_RESULTS];

static bool tag_name(const char *tag, const char *name, bool closing)
{
    const char *cursor = tag;
    if (closing != (*cursor == '/')) {
        return false;
    }
    if (closing) {
        cursor++;
    }
    const char *colon = strchr(cursor, ':');
    const char *space = strpbrk(cursor, " />\t\r\n");
    if (colon && (!space || colon < space)) {
        cursor = colon + 1;
    }
    size_t length = strcspn(cursor, " />\t\r\n");
    return strlen(name) == length && strncasecmp(cursor, name, length) == 0;
}

static void extract_href(const char *tag, char *output, size_t size)
{
    const char *href = strcasestr(tag, "href=");
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
    while (*end && *end != quote && !isspace((unsigned char)*end) && *end != '>') {
        end++;
    }
    if ((size_t)(end - href) < size) {
        memcpy(output, href, end - href);
        output[end - href] = 0;
    }
}

static void feed_start_text(feed_parser_t *parser, feed_field_t field, char *output, size_t size)
{
    parser->field = field;
    mp_html_text_init(&parser->text, output, size);
}

static void feed_tag(feed_parser_t *parser)
{
    parser->tag[parser->tag_length] = 0;
    if (tag_name(parser->tag, "entry", false) || tag_name(parser->tag, "item", false)) {
        memset(&parser->current, 0, sizeof(parser->current));
        parser->in_entry = true;
    } else if (parser->in_entry && tag_name(parser->tag, "title", false)) {
        feed_start_text(parser, FEED_TITLE, parser->current.title, sizeof(parser->current.title));
    } else if (parser->in_entry && (tag_name(parser->tag, "summary", false) ||
               tag_name(parser->tag, "description", false))) {
        feed_start_text(parser, FEED_SNIPPET, parser->current.snippet, sizeof(parser->current.snippet));
    } else if (parser->in_entry && tag_name(parser->tag, "id", false)) {
        feed_start_text(parser, FEED_URL, parser->current.url, sizeof(parser->current.url));
    } else if (parser->in_entry && tag_name(parser->tag, "link", false)) {
        extract_href(parser->tag, parser->current.url, sizeof(parser->current.url));
        if (!parser->current.url[0]) {
            feed_start_text(parser, FEED_URL, parser->current.url, sizeof(parser->current.url));
        }
    } else if (tag_name(parser->tag, "title", true) || tag_name(parser->tag, "summary", true) ||
               tag_name(parser->tag, "description", true) || tag_name(parser->tag, "id", true) ||
               tag_name(parser->tag, "link", true)) {
        mp_html_text_finish(&parser->text);
        parser->field = FEED_NONE;
    } else if (parser->in_entry && (tag_name(parser->tag, "entry", true) ||
               tag_name(parser->tag, "item", true))) {
        if (strncmp(parser->current.url, "http://arxiv.org/", 17) == 0) {
            memmove(parser->current.url + 8, parser->current.url + 7, strlen(parser->current.url + 7) + 1);
            memcpy(parser->current.url, "https://", 8);
        }
        if (parser->current.title[0] && parser->current.url[0] && parser->count < MP_SEARCH_MAX_RESULTS) {
            parser->results[parser->count++] = parser->current;
        }
        parser->in_entry = false;
        parser->field = FEED_NONE;
    }
    parser->tag_length = 0;
}

static void feed_cdata(feed_parser_t *parser, char value)
{
    if (value == ']') {
        if (parser->cdata_close < 2) {
            parser->cdata_close++;
        } else if (parser->field != FEED_NONE) {
            mp_html_text_push(&parser->text, ']');
        }
    } else if (value == '>' && parser->cdata_close == 2) {
        parser->cdata = false;
        parser->cdata_close = 0;
    } else {
        while (parser->cdata_close) {
            if (parser->field != FEED_NONE) {
                mp_html_text_push(&parser->text, ']');
            }
            parser->cdata_close--;
        }
        if (parser->field != FEED_NONE) {
            mp_html_text_push(&parser->text, value);
        }
    }
}

static bool feed_chunk(const uint8_t *data, size_t size, void *context)
{
    feed_parser_t *parser = context;
    for (size_t index = 0; index < size; index++) {
        char value = (char)data[index];
        if (parser->cdata) {
            feed_cdata(parser, value);
        } else if (parser->in_tag) {
            if (parser->tag_length == 8 && strncmp(parser->tag, "![CDATA[", 8) == 0) {
                parser->in_tag = false;
                parser->cdata = true;
                feed_cdata(parser, value);
            } else if (value == '>') {
                parser->in_tag = false;
                feed_tag(parser);
            } else if (parser->tag_length + 1 < sizeof(parser->tag)) {
                parser->tag[parser->tag_length++] = value;
            }
        } else if (value == '<') {
            parser->in_tag = true;
            parser->tag_length = 0;
        } else if (parser->field != FEED_NONE) {
            mp_html_text_push(&parser->text, value);
        }
        if (parser->count == MP_SEARCH_MAX_RESULTS) {
            return false;
        }
    }
    return true;
}

static esp_err_t feed_request(const char *url, mp_search_result_t *results, size_t *count)
{
    mp_http_header_t headers[] = {{"User-Agent", "MicroPaw/0.1 ESP32 personal assistant"}};
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .headers = headers,
        .header_count = 1,
        .response_limit = CONFIG_MICROPAW_SEARCH_DOWNLOAD_LIMIT,
        .timeout_ms = 25000,
        .accepted_content_types = "application/atom+xml,application/rss+xml,application/xml,text/xml"
    };
    feed_parser_t parser = {.results = results};
    mp_http_response_t response;
    esp_err_t error = mp_http_stream(&request, feed_chunk, &parser, &response);
    if (error != ESP_OK || response.status != 200) {
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }
    *count = parser.count;
    return parser.count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t arxiv_search(const char *query, mp_search_result_t *results, size_t *count)
{
    char terms[704];
    char encoded[768];
    char url[896];
    snprintf(terms, sizeof(terms), "all:%s", query);
    mp_url_encode(terms, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "https://export.arxiv.org/api/query?search_query=%s&start=0&max_results=5", encoded);
    return feed_request(url, results, count);
}

esp_err_t mp_rss_read(const char *url, char *output, size_t size)
{
    if (!mp_url_is_public_https(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_feed_results, 0, sizeof(s_feed_results));
    size_t count = 0;
    esp_err_t error = feed_request(url, s_feed_results, &count);
    if (error == ESP_OK) {
        mp_search_allow_results(s_feed_results, count);
        mp_search_format_results(s_feed_results, count, output, size);
    }
    return error;
}
