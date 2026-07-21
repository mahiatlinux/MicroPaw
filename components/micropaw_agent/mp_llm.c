#include "mp_llm.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "mp_config.h"
#include "mp_json.h"
#include "mp_net.h"
#include "sdkconfig.h"

typedef struct {
    mp_llm_result_t *result;
    char line[4096];
    size_t line_length;
    bool overflow;
} sse_parser_t;

EXT_RAM_BSS_ATTR static sse_parser_t s_parser;
EXT_RAM_BSS_ATTR static char s_value[4096];

static void append_text(char *target, size_t size, const char *text);
static bool provider(const mp_config_t *config, const char **url, bool *key_required);
static void parse_event(mp_llm_result_t *result, const char *data);
static void process_line(sse_parser_t *parser);
static bool sse_chunk(const uint8_t *data, size_t size, void *context);
bool mp_llm_ready(void);
esp_err_t mp_llm_stream(const char *body, mp_llm_result_t *result);

static void append_text(char *target, size_t size, const char *text)
{
    size_t used = strnlen(target, size);
    if (used < size) {
        strlcpy(target + used, text, size - used);
    }
}

static bool provider(const mp_config_t *config, const char **url, bool *key_required)
{
    *key_required = true;
    if (strcmp(config->llm_provider, "openai") == 0) {
        *url = "https://api.openai.com/v1/responses";
        return true;
    }
    if (strcmp(config->llm_provider, "openrouter") == 0) {
        *url = "https://openrouter.ai/api/v1/responses";
        return true;
    }
    if (strcmp(config->llm_provider, "openai_compatible") == 0 &&
        mp_url_is_https(config->llm_endpoint)) {
        *url = config->llm_endpoint;
        *key_required = false;
        return true;
    }
    return false;
}

static void parse_event(mp_llm_result_t *result, const char *data)
{
    if (strcmp(data, "[DONE]") == 0) {
        return;
    }
    size_t length = strlen(data);
    char type[64];
    if (!mp_json_get_string(data, length, "type", type, sizeof(type))) {
        return;
    }
    if (strcmp(type, "response.output_text.delta") == 0 ||
        strcmp(type, "response.content_part.delta") == 0) {
        if (mp_json_get_string(data, length, "delta", s_value, sizeof(s_value))) {
            append_text(result->text, sizeof(result->text), s_value);
        }
    } else if (strcmp(type, "response.output_item.added") == 0 ||
               strcmp(type, "response.output_item.done") == 0) {
        const char *item;
        size_t item_length;
        char item_type[32];
        if (mp_json_get_slice(data, length, "item", &item, &item_length) &&
            mp_json_get_string(item, item_length, "type", item_type, sizeof(item_type)) &&
            strcmp(item_type, "function_call") == 0) {
            result->has_tool = true;
            mp_json_get_string(item, item_length, "id", result->item_id, sizeof(result->item_id));
            mp_json_get_string(item, item_length, "name", result->tool, sizeof(result->tool));
            mp_json_get_string(item, item_length, "call_id", result->call_id, sizeof(result->call_id));
            mp_json_get_string(item, item_length, "arguments", result->arguments,
                               sizeof(result->arguments));
        }
    } else if (strcmp(type, "response.function_call_arguments.delta") == 0) {
        if (mp_json_get_string(data, length, "delta", s_value, sizeof(s_value))) {
            append_text(result->arguments, sizeof(result->arguments), s_value);
        }
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        mp_json_get_string(data, length, "arguments", result->arguments, sizeof(result->arguments));
    } else if (strcmp(type, "error") == 0) {
        const char *error;
        size_t error_length;
        if (!mp_json_get_string(data, length, "message", result->error, sizeof(result->error)) &&
            mp_json_get_slice(data, length, "error", &error, &error_length)) {
            mp_json_get_string(error, error_length, "message", result->error, sizeof(result->error));
        }
    } else if (strcmp(type, "response.failed") == 0) {
        const char *response;
        const char *error;
        size_t response_length;
        size_t error_length;
        bool found = mp_json_get_slice(data, length, "response", &response, &response_length) &&
                     mp_json_get_slice(response, response_length, "error", &error, &error_length) &&
                     mp_json_get_string(error, error_length, "message", result->error, sizeof(result->error));
        if (!found) {
            strlcpy(result->error, "The inference request failed.", sizeof(result->error));
        }
    }
}

static void process_line(sse_parser_t *parser)
{
    parser->line[parser->line_length] = 0;
    if (!parser->overflow && strncmp(parser->line, "data:", 5) == 0) {
        const char *data = parser->line + 5;
        while (*data == ' ') {
            data++;
        }
        parse_event(parser->result, data);
    }
    parser->line_length = 0;
    parser->overflow = false;
}

static bool sse_chunk(const uint8_t *data, size_t size, void *context)
{
    sse_parser_t *parser = context;
    for (size_t index = 0; index < size; index++) {
        char value = (char)data[index];
        if (value == '\n') {
            process_line(parser);
        } else if (value != '\r' && !parser->overflow) {
            if (parser->line_length + 1 < sizeof(parser->line)) {
                parser->line[parser->line_length++] = value;
            } else {
                parser->overflow = true;
            }
        }
    }
    return true;
}

bool mp_llm_ready(void)
{
    const mp_config_t *config = mp_config_get();
    const char *url;
    bool key_required;
    return config->llm_model[0] && provider(config, &url, &key_required) &&
           (!key_required || config->llm_api_key[0]);
}

esp_err_t mp_llm_stream(const char *body, mp_llm_result_t *result)
{
    const mp_config_t *config = mp_config_get();
    const char *url;
    bool key_required;
    if (!body || !result || !config->llm_model[0] || !provider(config, &url, &key_required) ||
        (key_required && !config->llm_api_key[0])) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(result, 0, sizeof(*result));
    memset(&s_parser, 0, sizeof(s_parser));
    s_parser.result = result;
    char authorization[272];
    mp_http_header_t headers[3] = {
        {"Content-Type", "application/json"},
        {"Accept", "text/event-stream"}
    };
    size_t header_count = 2;
    if (config->llm_api_key[0]) {
        snprintf(authorization, sizeof(authorization), "Bearer %s", config->llm_api_key);
        headers[header_count++] = (mp_http_header_t){"Authorization", authorization};
    }
    mp_http_request_t request = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_size = strlen(body),
        .response_limit = CONFIG_MICROPAW_LLM_STREAM_LIMIT,
        .timeout_ms = 60000,
        .accepted_content_types = "text/event-stream,application/json"
    };
    mp_http_response_t response;
    esp_err_t error = mp_http_stream(&request, sse_chunk, &s_parser, &response);
    if (s_parser.line_length) {
        process_line(&s_parser);
    }
    if (error != ESP_OK) {
        return error;
    }
    if (response.status != 200) {
        snprintf(result->error, sizeof(result->error), "Inference HTTP status %d.", response.status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (result->error[0]) {
        return ESP_FAIL;
    }
    if (result->has_tool && (!result->item_id[0] || !result->tool[0] || !result->call_id[0] ||
                            !result->arguments[0])) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return result->has_tool || result->text[0] ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
