#include "mp_search.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "mp_config.h"
#include "mp_json.h"
#include "mp_net.h"
#include "sdkconfig.h"

#include "mp_feed.h"

extern const mp_search_provider_t mp_wikipedia_provider;

typedef enum {
    BRAVE_FIELD_NONE,
    BRAVE_FIELD_TITLE,
    BRAVE_FIELD_URL,
    BRAVE_FIELD_DESCRIPTION
} brave_field_t;

typedef struct {
    mp_search_result_t *results;
    size_t count;
    char token[1024];
    size_t token_length;
    int depth;
    int web_depth;
    int results_depth;
    int result_depth;
    brave_field_t field;
    bool quoted;
    bool escaped;
    bool key;
    bool value;
    bool expect_key;
    bool expect_value;
    bool overflow;
    bool pending_web;
    bool pending_results;
    bool finished;
} brave_parser_t;

EXT_RAM_BSS_ATTR static char s_allowed[MP_SEARCH_MAX_RESULTS][MP_SEARCH_URL_LEN];
EXT_RAM_BSS_ATTR static mp_search_result_t s_results[MP_SEARCH_MAX_RESULTS];
EXT_RAM_BSS_ATTR static char s_encoded[4801];
EXT_RAM_BSS_ATTR static char s_url[5120];

static void brave_token(brave_parser_t *parser, char value);
static void brave_string(brave_parser_t *parser);
static void brave_result(brave_parser_t *parser);
static bool brave_chunk(const uint8_t *data, size_t size, void *context);
static bool brave_query_valid(const char *query);
static esp_err_t brave_status(int status, char *error_text, size_t error_size);
static esp_err_t brave_search(const char *query, mp_search_result_t *results, size_t *count,
                              char *error_text, size_t error_size);
const mp_search_provider_t *mp_search_provider(const char *name);
esp_err_t mp_search_run(const char *provider, const char *query, char *output, size_t size);
bool mp_search_url_allowed(const char *url);

static const mp_search_provider_t s_brave_provider = {"brave", brave_search};

static const mp_search_provider_t *const s_providers[] = {
    &s_brave_provider,
#if CONFIG_MICROPAW_WIKIPEDIA
    &mp_wikipedia_provider,
#endif
#if CONFIG_MICROPAW_ARXIV
    &mp_arxiv_provider,
#endif
};

static void brave_token(brave_parser_t *parser, char value)
{
    if (parser->overflow) {
        return;
    }
    if (parser->token_length + 1 >= sizeof(parser->token)) {
        parser->overflow = true;
        return;
    }
    parser->token[parser->token_length++] = value;
}

static void brave_string(brave_parser_t *parser)
{
    if (!parser->overflow) {
        parser->token[parser->token_length] = 0;
    }
    if (parser->key) {
        if (!parser->overflow && parser->result_depth) {
            parser->field = strcmp(parser->token, "title") == 0 ? BRAVE_FIELD_TITLE :
                            strcmp(parser->token, "url") == 0 ? BRAVE_FIELD_URL :
                            strcmp(parser->token, "description") == 0 ?
                            BRAVE_FIELD_DESCRIPTION : BRAVE_FIELD_NONE;
        } else if (!parser->overflow && parser->depth == 1 &&
                   strcmp(parser->token, "web") == 0) {
            parser->pending_web = true;
        } else if (!parser->overflow && parser->web_depth &&
                   parser->depth == parser->web_depth &&
                   strcmp(parser->token, "results") == 0) {
            parser->pending_results = true;
        }
        parser->expect_key = false;
    } else if (parser->value) {
        if (!parser->overflow && parser->field != BRAVE_FIELD_NONE) {
            mp_search_result_t *result = &parser->results[parser->count];
            char *target = parser->field == BRAVE_FIELD_TITLE ? result->title :
                           parser->field == BRAVE_FIELD_URL ? result->url : result->snippet;
            size_t size = parser->field == BRAVE_FIELD_TITLE ? sizeof(result->title) :
                          parser->field == BRAVE_FIELD_URL ? sizeof(result->url) :
                          sizeof(result->snippet);
            bool decoded = mp_json_decode_string(parser->token, parser->token_length,
                                                 target, size);
            if (!decoded && parser->field != BRAVE_FIELD_DESCRIPTION) {
                target[0] = 0;
            }
        }
        parser->expect_value = false;
        parser->field = BRAVE_FIELD_NONE;
    }
    parser->key = false;
    parser->value = false;
}

static void brave_result(brave_parser_t *parser)
{
    mp_search_result_t *result = &parser->results[parser->count];
    if (result->title[0] && result->url[0]) {
        parser->count++;
    } else {
        memset(result, 0, sizeof(*result));
    }
    parser->result_depth = 0;
    parser->field = BRAVE_FIELD_NONE;
    parser->expect_key = false;
    parser->expect_value = false;
}

static bool brave_chunk(const uint8_t *data, size_t size, void *context)
{
    brave_parser_t *parser = context;
    for (size_t index = 0; index < size; index++) {
        char value = (char)data[index];
        if (parser->quoted) {
            if (parser->escaped) {
                if (parser->key || (parser->value && parser->field != BRAVE_FIELD_NONE)) {
                    brave_token(parser, value);
                }
                parser->escaped = false;
            } else if (value == '\\') {
                if (parser->key || (parser->value && parser->field != BRAVE_FIELD_NONE)) {
                    brave_token(parser, value);
                }
                parser->escaped = true;
            } else if (value == '"') {
                if (parser->value && parser->field != BRAVE_FIELD_NONE) {
                    brave_token(parser, value);
                }
                parser->quoted = false;
                brave_string(parser);
            } else if (parser->key || (parser->value && parser->field != BRAVE_FIELD_NONE)) {
                brave_token(parser, value);
            }
            continue;
        }
        if (value == '"') {
            bool key = parser->expect_key &&
                       (parser->depth == 1 ||
                        (parser->web_depth && parser->depth == parser->web_depth) ||
                        (parser->result_depth && parser->depth == parser->result_depth));
            bool string_value = parser->expect_value && parser->result_depth &&
                                parser->depth == parser->result_depth;
            parser->quoted = true;
            parser->key = key;
            parser->value = string_value;
            parser->token_length = 0;
            parser->overflow = false;
            if (string_value && parser->field != BRAVE_FIELD_NONE) {
                brave_token(parser, value);
            }
            continue;
        }
        if (value == '{') {
            if (parser->results_depth && !parser->result_depth &&
                parser->depth == parser->results_depth) {
                parser->result_depth = parser->depth + 1;
                memset(&parser->results[parser->count], 0,
                       sizeof(parser->results[parser->count]));
            } else if (parser->pending_web && parser->depth == 1) {
                parser->web_depth = parser->depth + 1;
            }
            parser->depth++;
            parser->expect_key = parser->depth == 1 ||
                                 parser->depth == parser->web_depth ||
                                 parser->depth == parser->result_depth;
            parser->expect_value = false;
            parser->pending_web = false;
            parser->pending_results = false;
        } else if (value == '}') {
            if (parser->result_depth && parser->depth == parser->result_depth) {
                brave_result(parser);
                if (parser->count == MP_SEARCH_MAX_RESULTS) {
                    parser->finished = true;
                    return false;
                }
            } else if (parser->web_depth && parser->depth == parser->web_depth) {
                parser->web_depth = 0;
            }
            parser->depth--;
        } else if (value == '[') {
            if (parser->pending_results && parser->web_depth &&
                parser->depth == parser->web_depth) {
                parser->results_depth = parser->depth + 1;
            }
            parser->depth++;
            parser->expect_value = false;
            parser->pending_results = false;
        } else if (value == ']') {
            if (parser->results_depth && parser->depth == parser->results_depth) {
                parser->finished = true;
                return false;
            }
            parser->depth--;
        } else if (value == ':') {
            if (!parser->expect_key) {
                parser->expect_value = true;
            }
        } else if (value == ',') {
            if (parser->depth == 1 ||
                (parser->web_depth && parser->depth == parser->web_depth) ||
                (parser->result_depth && parser->depth == parser->result_depth)) {
                parser->expect_key = true;
                parser->expect_value = false;
                parser->field = BRAVE_FIELD_NONE;
                parser->pending_web = false;
                parser->pending_results = false;
            }
        } else if (!isspace((unsigned char)value) && value != ':' &&
                   parser->expect_value && parser->result_depth &&
                   parser->depth == parser->result_depth) {
            parser->expect_value = false;
            parser->field = BRAVE_FIELD_NONE;
        }
    }
    return true;
}

static bool brave_query_valid(const char *query)
{
    size_t length = strlen(query);
    size_t characters = 0;
    size_t words = 0;
    bool word = false;
    if (!length || length > 1600) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        if (((unsigned char)query[index] & 0xc0) != 0x80 && ++characters > 400) {
            return false;
        }
        if (isspace((unsigned char)query[index])) {
            word = false;
        } else if (!word) {
            word = true;
            if (++words > 50) {
                return false;
            }
        }
    }
    return true;
}

static esp_err_t brave_status(int status, char *error_text, size_t error_size)
{
    if (status == 401 || status == 403) {
        strlcpy(error_text, "Brave Search rejected the API key.", error_size);
    } else if (status == 422) {
        strlcpy(error_text, "Brave Search rejected the query.", error_size);
    } else if (status == 429) {
        strlcpy(error_text, "Brave Search quota or rate limit reached.", error_size);
    } else if (status >= 500) {
        strlcpy(error_text, "Brave Search is temporarily unavailable.", error_size);
    } else {
        snprintf(error_text, error_size, "Brave Search returned HTTP %d.", status);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t brave_search(const char *query, mp_search_result_t *results, size_t *count,
                              char *error_text, size_t error_size)
{
    const mp_config_t *config = mp_config_get();
    const char *key = config->brave_api_key;
    if (!key[0]) {
        strlcpy(error_text, "Brave Search API key is not configured.", error_size);
        return ESP_ERR_INVALID_STATE;
    }
    if (!brave_query_valid(query)) {
        strlcpy(error_text, "Brave Search queries must contain at most 400 characters and 50 words.",
                error_size);
        return ESP_ERR_INVALID_ARG;
    }
    mp_url_encode(query, s_encoded, sizeof(s_encoded));
    int written = snprintf(
        s_url, sizeof(s_url),
        "https://api.search.brave.com/res/v1/web/search?q=%s&count=5&result_filter=web&text_decorations=false%s%s%s%s",
        s_encoded, config->brave_country[0] ? "&country=" : "", config->brave_country,
        config->brave_search_lang[0] ? "&search_lang=" : "", config->brave_search_lang);
    if (written < 0 || (size_t)written >= sizeof(s_url)) {
        strlcpy(error_text, "Brave Search query URL is too long.", error_size);
        return ESP_ERR_INVALID_SIZE;
    }
    mp_http_header_t headers[] = {
        {"X-Subscription-Token", key},
        {"Accept", "application/json"},
        {"User-Agent", "MicroPaw/0.1 ESP32"}
    };
    mp_http_request_t request = {
        .url = s_url,
        .method = HTTP_METHOD_GET,
        .headers = headers,
        .header_count = sizeof(headers) / sizeof(headers[0]),
        .response_limit = CONFIG_MICROPAW_SEARCH_DOWNLOAD_LIMIT,
        .timeout_ms = 20000,
        .accepted_content_types = "application/json"
    };
    brave_parser_t parser = {.results = results};
    mp_http_response_t response;
    esp_err_t error = mp_http_stream(&request, brave_chunk, &parser, &response);
    if (error != ESP_OK) {
        snprintf(error_text, error_size, "Brave Search request failed: %s.",
                 esp_err_to_name(error));
        return error;
    }
    if (response.status != 200) {
        return brave_status(response.status, error_text, error_size);
    }
    if (!parser.finished) {
        strlcpy(error_text, "Brave Search returned an incomplete response.", error_size);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *count = parser.count;
    if (!parser.count) {
        strlcpy(error_text, "Brave Search returned no web results.", error_size);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
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
    output[0] = 0;
    size_t count = 0;
    esp_err_t error = selected->search(query, s_results, &count, output, size);
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
