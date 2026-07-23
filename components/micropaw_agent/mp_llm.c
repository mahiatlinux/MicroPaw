#include "mp_llm.h"

#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "mp_config.h"
#include "mp_metrics.h"
#include "mp_net.h"
#include "mp_wifi.h"
#include "sdkconfig.h"

typedef enum {
    CAPTURE_NONE,
    CAPTURE_KEY,
    CAPTURE_EVENT_TYPE,
    CAPTURE_ITEM_TYPE,
    CAPTURE_ITEM_ID,
    CAPTURE_CALL_ID,
    CAPTURE_NAME,
    CAPTURE_ARGUMENTS,
    CAPTURE_TEXT,
    CAPTURE_ERROR
} capture_t;

typedef struct {
    mp_llm_result_t *result;
    mp_llm_call_t *scratch;
    mp_llm_call_t *pending;
    size_t scratch_offset;
    size_t arguments_length;
    size_t key_length;
    size_t event_type_length;
    size_t item_type_length;
    size_t item_id_length;
    size_t call_id_length;
    size_t name_length;
    size_t error_length;
    unsigned depth;
    unsigned unicode_digits;
    uint32_t unicode_value;
    uint16_t high_surrogate;
    capture_t capture;
    char key[48];
    char event_type[64];
    char item_type[32];
    char item_id[MP_ITEM_ID_LEN];
    bool in_string;
    bool value_string;
    bool after_colon;
    bool escape;
    bool overflow;
    bool line_ignored;
    bool data_line;
    bool data_started;
    uint8_t prefix_length;
} sse_parser_t;

static sse_parser_t s_parser;
static mp_http_session_t s_session;
static char s_boot_id[33];

static bool provider(const mp_config_t *config, const char **url, bool *key_required);
static uint8_t *arena_data(mp_llm_result_t *result);
static const uint8_t *const_arena_data(const mp_llm_result_t *result);
static mp_llm_call_t *find_call(mp_llm_result_t *result, const char *item_id,
                                mp_llm_call_state_t state);
static mp_llm_call_t *start_scratch(sse_parser_t *parser);
static void discard_scratch(sse_parser_t *parser);
static bool commit_scratch(sse_parser_t *parser, mp_llm_call_state_t state);
static void event_reset(sse_parser_t *parser);
static void event_finish(sse_parser_t *parser);
static capture_t value_capture(sse_parser_t *parser);
static void terminate_capture(sse_parser_t *parser);
static bool emit_byte(sse_parser_t *parser, uint8_t value);
static bool emit_codepoint(sse_parser_t *parser, uint32_t value);
static int hex_value(char value);
static void string_byte(sse_parser_t *parser, char value);
static void json_byte(sse_parser_t *parser, char value);
static void line_finish(sse_parser_t *parser);
static void sse_byte(sse_parser_t *parser, char value);
static bool sse_chunk(const uint8_t *data, size_t size, void *context);
esp_err_t mp_llm_init(void);
bool mp_llm_ready(void);
const char *mp_llm_boot_id(void);
void mp_llm_parse_begin(mp_llm_result_t *result);
bool mp_llm_parse_chunk(mp_llm_result_t *result, const uint8_t *data, size_t size);
esp_err_t mp_llm_parse_finish(mp_llm_result_t *result);
const mp_llm_call_t *mp_llm_call_next(const mp_llm_result_t *result, size_t *offset);
esp_err_t mp_llm_stream(const char *body, mp_llm_result_t *result);

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

static uint8_t *arena_data(mp_llm_result_t *result)
{
    return (uint8_t *)result->arena;
}

static const uint8_t *const_arena_data(const mp_llm_result_t *result)
{
    return (const uint8_t *)result->arena;
}

static mp_llm_call_t *find_call(mp_llm_result_t *result, const char *item_id,
                                mp_llm_call_state_t state)
{
    size_t offset = 0;
    while (offset < result->arena_used) {
        mp_llm_call_t *call = (mp_llm_call_t *)(arena_data(result) + offset);
        if (call->state == state && strcmp(call->item_id, item_id) == 0) {
            return call;
        }
        offset += call->size;
    }
    return NULL;
}

static mp_llm_call_t *start_scratch(sse_parser_t *parser)
{
    if (parser->scratch) {
        return parser->scratch;
    }
    size_t available = MP_LLM_CALL_ARENA_LEN - parser->result->arena_used;
    if (available <= sizeof(mp_llm_call_t) + 1) {
        parser->overflow = true;
        return NULL;
    }
    parser->scratch_offset = parser->result->arena_used;
    parser->scratch = (mp_llm_call_t *)(arena_data(parser->result) + parser->scratch_offset);
    memset(parser->scratch, 0, sizeof(*parser->scratch));
    parser->arguments_length = 0;
    return parser->scratch;
}

static void discard_scratch(sse_parser_t *parser)
{
    if (parser->scratch) {
        size_t available = MP_LLM_CALL_ARENA_LEN - parser->scratch_offset;
        size_t cleared = sizeof(*parser->scratch) + parser->arguments_length + 1;
        memset(parser->scratch, 0, cleared < available ? cleared : available);
    }
    parser->scratch = NULL;
    parser->arguments_length = 0;
}

static bool commit_scratch(sse_parser_t *parser, mp_llm_call_state_t state)
{
    mp_llm_call_t *call = parser->scratch;
    if (!call || parser->overflow) {
        discard_scratch(parser);
        return false;
    }
    call->arguments[parser->arguments_length] = 0;
    call->arguments_length = parser->arguments_length;
    size_t size = sizeof(*call) + parser->arguments_length + 1;
    size = (size + 3) & ~3U;
    if (size > MP_LLM_CALL_ARENA_LEN - parser->scratch_offset) {
        parser->overflow = true;
        discard_scratch(parser);
        return false;
    }
    call->size = size;
    call->state = state;
    if (!call->order) {
        call->order = ++parser->result->next_call_order;
    }
    parser->result->arena_used += size;
    if (state == MP_LLM_CALL_READY) {
        parser->result->call_count++;
    }
    parser->scratch = NULL;
    parser->arguments_length = 0;
    return true;
}

static void event_reset(sse_parser_t *parser)
{
    parser->scratch = NULL;
    parser->pending = NULL;
    parser->arguments_length = 0;
    parser->key_length = 0;
    parser->event_type_length = 0;
    parser->item_type_length = 0;
    parser->item_id_length = 0;
    parser->call_id_length = 0;
    parser->name_length = 0;
    parser->error_length = 0;
    parser->depth = 0;
    parser->unicode_digits = 0;
    parser->unicode_value = 0;
    parser->high_surrogate = 0;
    parser->capture = CAPTURE_NONE;
    parser->key[0] = 0;
    parser->event_type[0] = 0;
    parser->item_type[0] = 0;
    parser->item_id[0] = 0;
    parser->in_string = false;
    parser->value_string = false;
    parser->after_colon = false;
    parser->escape = false;
    parser->overflow = false;
}

static void event_finish(sse_parser_t *parser)
{
    if (parser->scratch) {
        if ((strcmp(parser->event_type, "response.output_item.added") == 0 ||
             strcmp(parser->event_type, "response.output_item.done") == 0) &&
            strcmp(parser->item_type, "message") == 0) {
            if (parser->scratch->item_id[0]) {
                strlcpy(parser->result->message_id, parser->scratch->item_id,
                        sizeof(parser->result->message_id));
            }
            discard_scratch(parser);
        } else if (strcmp(parser->item_type, "function_call") == 0) {
            bool done = strcmp(parser->event_type, "response.output_item.done") == 0;
            mp_llm_call_t *final = find_call(parser->result, parser->scratch->item_id,
                                             MP_LLM_CALL_READY);
            if (final) {
                discard_scratch(parser);
            } else {
                parser->pending = find_call(parser->result, parser->scratch->item_id,
                                            MP_LLM_CALL_PENDING);
                if (parser->pending) {
                    parser->scratch->order = parser->pending->order;
                    if (!parser->scratch->call_id[0]) {
                        strlcpy(parser->scratch->call_id, parser->pending->call_id,
                                sizeof(parser->scratch->call_id));
                    }
                    if (!parser->scratch->name[0]) {
                        strlcpy(parser->scratch->name, parser->pending->name,
                                sizeof(parser->scratch->name));
                    }
                    if (done) {
                        parser->pending->state = MP_LLM_CALL_DISCARDED;
                    }
                }
                commit_scratch(parser, done ? MP_LLM_CALL_READY : MP_LLM_CALL_PENDING);
            }
        } else if (strcmp(parser->event_type,
                          "response.function_call_arguments.done") == 0) {
            mp_llm_call_t *final = find_call(parser->result, parser->item_id,
                                             MP_LLM_CALL_READY);
            parser->pending = find_call(parser->result, parser->item_id,
                                        MP_LLM_CALL_PENDING);
            if (final || !parser->pending) {
                discard_scratch(parser);
            } else {
                strlcpy(parser->scratch->item_id, parser->pending->item_id,
                        sizeof(parser->scratch->item_id));
                strlcpy(parser->scratch->call_id, parser->pending->call_id,
                        sizeof(parser->scratch->call_id));
                strlcpy(parser->scratch->name, parser->pending->name,
                        sizeof(parser->scratch->name));
                parser->scratch->order = parser->pending->order;
                parser->pending->state = MP_LLM_CALL_DISCARDED;
                commit_scratch(parser, MP_LLM_CALL_READY);
            }
        } else {
            discard_scratch(parser);
        }
    }
    if (parser->overflow && !parser->result->error[0]) {
        strlcpy(parser->result->error, "Inference tool batch exceeded call arena capacity.",
                sizeof(parser->result->error));
    }
    event_reset(parser);
}

static capture_t value_capture(sse_parser_t *parser)
{
    if (strcmp(parser->key, "type") == 0) {
        return parser->depth == 1 ? CAPTURE_EVENT_TYPE :
               parser->depth == 2 ? CAPTURE_ITEM_TYPE : CAPTURE_NONE;
    }
    if ((strcmp(parser->event_type, "response.output_text.delta") == 0 ||
         strcmp(parser->event_type, "response.content_part.delta") == 0) &&
        strcmp(parser->key, "delta") == 0) {
        return CAPTURE_TEXT;
    }
    if ((strcmp(parser->event_type, "error") == 0 ||
         strcmp(parser->event_type, "response.failed") == 0) &&
        strcmp(parser->key, "message") == 0) {
        return CAPTURE_ERROR;
    }
    if (strcmp(parser->event_type, "response.function_call_arguments.done") == 0) {
        if (strcmp(parser->key, "item_id") == 0) {
            return CAPTURE_ITEM_ID;
        }
        if (strcmp(parser->key, "arguments") == 0 && start_scratch(parser)) {
            return CAPTURE_ARGUMENTS;
        }
    }
    if ((strcmp(parser->event_type, "response.output_item.added") == 0 ||
         strcmp(parser->event_type, "response.output_item.done") == 0) &&
        parser->depth >= 2 && start_scratch(parser)) {
        if (strcmp(parser->key, "id") == 0) {
            return CAPTURE_ITEM_ID;
        }
        if (strcmp(parser->key, "call_id") == 0) {
            return CAPTURE_CALL_ID;
        }
        if (strcmp(parser->key, "name") == 0) {
            return CAPTURE_NAME;
        }
        if (strcmp(parser->key, "arguments") == 0) {
            return CAPTURE_ARGUMENTS;
        }
    }
    return CAPTURE_NONE;
}

static void terminate_capture(sse_parser_t *parser)
{
    switch (parser->capture) {
        case CAPTURE_KEY:
            parser->key[parser->key_length] = 0;
            break;
        case CAPTURE_EVENT_TYPE:
            parser->event_type[parser->event_type_length] = 0;
            break;
        case CAPTURE_ITEM_TYPE:
            parser->item_type[parser->item_type_length] = 0;
            break;
        case CAPTURE_ITEM_ID:
            if (parser->scratch) {
                parser->scratch->item_id[parser->item_id_length] = 0;
            } else {
                parser->item_id[parser->item_id_length] = 0;
            }
            break;
        case CAPTURE_CALL_ID:
            parser->scratch->call_id[parser->call_id_length] = 0;
            break;
        case CAPTURE_NAME:
            parser->scratch->name[parser->name_length] = 0;
            break;
        case CAPTURE_ARGUMENTS:
            if (parser->scratch && parser->arguments_length <
                MP_LLM_CALL_ARENA_LEN - parser->scratch_offset - sizeof(*parser->scratch)) {
                parser->scratch->arguments[parser->arguments_length] = 0;
            }
            break;
        case CAPTURE_TEXT:
            parser->result->text[parser->result->text_length] = 0;
            break;
        case CAPTURE_ERROR:
            parser->result->error[parser->error_length] = 0;
            break;
        default:
            break;
    }
}

static bool emit_byte(sse_parser_t *parser, uint8_t value)
{
    switch (parser->capture) {
        case CAPTURE_KEY:
            if (parser->key_length + 1 >= sizeof(parser->key)) {
                return false;
            }
            parser->key[parser->key_length++] = value;
            return true;
        case CAPTURE_EVENT_TYPE:
            if (parser->event_type_length + 1 >= sizeof(parser->event_type)) {
                return false;
            }
            parser->event_type[parser->event_type_length++] = value;
            return true;
        case CAPTURE_ITEM_TYPE:
            if (parser->item_type_length + 1 >= sizeof(parser->item_type)) {
                return false;
            }
            parser->item_type[parser->item_type_length++] = value;
            return true;
        case CAPTURE_ITEM_ID:
            if (parser->item_id_length + 1 >= MP_ITEM_ID_LEN) {
                return false;
            }
            if (parser->scratch) {
                parser->scratch->item_id[parser->item_id_length++] = value;
            } else {
                parser->item_id[parser->item_id_length++] = value;
            }
            return true;
        case CAPTURE_CALL_ID:
            if (parser->call_id_length + 1 >= MP_CALL_ID_LEN) {
                return false;
            }
            parser->scratch->call_id[parser->call_id_length++] = value;
            return true;
        case CAPTURE_NAME:
            if (parser->name_length + 1 >= MP_TOOL_NAME_LEN) {
                return false;
            }
            parser->scratch->name[parser->name_length++] = value;
            return true;
        case CAPTURE_ARGUMENTS: {
            size_t available = MP_LLM_CALL_ARENA_LEN - parser->scratch_offset -
                               sizeof(*parser->scratch);
            if (parser->arguments_length + 1 >= available) {
                parser->overflow = true;
                return false;
            }
            parser->scratch->arguments[parser->arguments_length++] = value;
            return true;
        }
        case CAPTURE_TEXT:
            if (parser->result->text_length + 1 >= sizeof(parser->result->text)) {
                strlcpy(parser->result->error, "Inference response exceeded device capacity.",
                        sizeof(parser->result->error));
                return false;
            }
            parser->result->text[parser->result->text_length++] = value;
            return true;
        case CAPTURE_ERROR:
            if (parser->error_length + 1 >= sizeof(parser->result->error)) {
                return false;
            }
            parser->result->error[parser->error_length++] = value;
            return true;
        default:
            return true;
    }
}

static bool emit_codepoint(sse_parser_t *parser, uint32_t value)
{
    if (value <= 0x7f) {
        return emit_byte(parser, value);
    }
    if (value <= 0x7ff) {
        return emit_byte(parser, 0xc0 | (value >> 6)) &&
               emit_byte(parser, 0x80 | (value & 0x3f));
    }
    if (value <= 0xffff) {
        return emit_byte(parser, 0xe0 | (value >> 12)) &&
               emit_byte(parser, 0x80 | ((value >> 6) & 0x3f)) &&
               emit_byte(parser, 0x80 | (value & 0x3f));
    }
    return emit_byte(parser, 0xf0 | (value >> 18)) &&
           emit_byte(parser, 0x80 | ((value >> 12) & 0x3f)) &&
           emit_byte(parser, 0x80 | ((value >> 6) & 0x3f)) &&
           emit_byte(parser, 0x80 | (value & 0x3f));
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static void string_byte(sse_parser_t *parser, char value)
{
    if (parser->unicode_digits) {
        int digit = hex_value(value);
        if (digit < 0) {
            parser->overflow = true;
            return;
        }
        parser->unicode_value = (parser->unicode_value << 4) | digit;
        if (--parser->unicode_digits == 0) {
            uint32_t codepoint = parser->unicode_value;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                parser->high_surrogate = codepoint;
            } else {
                if (codepoint >= 0xdc00 && codepoint <= 0xdfff &&
                    parser->high_surrogate) {
                    codepoint = 0x10000 + ((parser->high_surrogate - 0xd800) << 10) +
                                codepoint - 0xdc00;
                    parser->high_surrogate = 0;
                } else if (parser->high_surrogate) {
                    emit_codepoint(parser, 0xfffd);
                    parser->high_surrogate = 0;
                }
                emit_codepoint(parser, codepoint);
            }
            parser->unicode_value = 0;
        }
        return;
    }
    if (parser->escape) {
        parser->escape = false;
        if (value == 'u') {
            parser->unicode_digits = 4;
            parser->unicode_value = 0;
            return;
        }
        char decoded = value;
        if (value == 'n') {
            decoded = '\n';
        } else if (value == 'r') {
            decoded = '\r';
        } else if (value == 't') {
            decoded = '\t';
        } else if (value == 'b') {
            decoded = '\b';
        } else if (value == 'f') {
            decoded = '\f';
        }
        emit_byte(parser, decoded);
        return;
    }
    if (value == '\\') {
        parser->escape = true;
        return;
    }
    if (value == '"') {
        if (parser->high_surrogate) {
            emit_codepoint(parser, 0xfffd);
            parser->high_surrogate = 0;
        }
        terminate_capture(parser);
        parser->in_string = false;
        parser->after_colon = false;
        parser->capture = CAPTURE_NONE;
        return;
    }
    emit_byte(parser, value);
}

static void json_byte(sse_parser_t *parser, char value)
{
    if (parser->in_string) {
        string_byte(parser, value);
        return;
    }
    if (value == '{' || value == '[') {
        parser->depth++;
        if (parser->after_colon) {
            parser->after_colon = false;
        }
    } else if (value == '}' || value == ']') {
        if (parser->depth) {
            parser->depth--;
        }
        parser->after_colon = false;
    } else if (value == ':') {
        parser->after_colon = true;
    } else if (value == ',') {
        parser->after_colon = false;
    } else if (value == '"') {
        parser->in_string = true;
        parser->value_string = parser->after_colon;
        parser->escape = false;
        parser->unicode_digits = 0;
        parser->high_surrogate = 0;
        if (parser->value_string) {
            parser->capture = value_capture(parser);
            parser->item_id_length = parser->capture == CAPTURE_ITEM_ID ? 0 :
                                     parser->item_id_length;
            parser->call_id_length = parser->capture == CAPTURE_CALL_ID ? 0 :
                                     parser->call_id_length;
            parser->name_length = parser->capture == CAPTURE_NAME ? 0 :
                                  parser->name_length;
            parser->error_length = parser->capture == CAPTURE_ERROR ? 0 :
                                   parser->error_length;
        } else {
            parser->capture = CAPTURE_KEY;
            parser->key_length = 0;
        }
    }
}

static void line_finish(sse_parser_t *parser)
{
    if (parser->data_line && parser->data_started && !parser->line_ignored) {
        event_finish(parser);
    } else {
        event_reset(parser);
    }
    parser->line_ignored = false;
    parser->data_line = false;
    parser->data_started = false;
    parser->prefix_length = 0;
}

static void sse_byte(sse_parser_t *parser, char value)
{
    static const char prefix[] = "data:";
    if (value == '\n') {
        line_finish(parser);
        return;
    }
    if (value == '\r' || parser->line_ignored) {
        return;
    }
    if (!parser->data_line) {
        if (parser->prefix_length < sizeof(prefix) - 1 &&
            value == prefix[parser->prefix_length]) {
            parser->prefix_length++;
            if (parser->prefix_length == sizeof(prefix) - 1) {
                parser->data_line = true;
                event_reset(parser);
            }
        } else {
            parser->line_ignored = true;
        }
        return;
    }
    if (!parser->data_started && value == ' ') {
        return;
    }
    parser->data_started = true;
    json_byte(parser, value);
}

static bool sse_chunk(const uint8_t *data, size_t size, void *context)
{
    return mp_llm_parse_chunk(context, data, size);
}

esp_err_t mp_llm_init(void)
{
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (size_t index = 0; index < sizeof(random); index++) {
        snprintf(s_boot_id + index * 2, 3, "%02x", random[index]);
    }
    return ESP_OK;
}

bool mp_llm_ready(void)
{
    const mp_config_t *config = mp_config_get();
    const char *url;
    bool key_required;
    return config->llm_model[0] && provider(config, &url, &key_required) &&
           (!key_required || config->llm_api_key[0]);
}

const char *mp_llm_boot_id(void)
{
    return s_boot_id;
}

void mp_llm_parse_begin(mp_llm_result_t *result)
{
    memset(result, 0, sizeof(*result));
    memset(&s_parser, 0, sizeof(s_parser));
    s_parser.result = result;
}

bool mp_llm_parse_chunk(mp_llm_result_t *result, const uint8_t *data, size_t size)
{
    if (!result || s_parser.result != result || (!data && size)) {
        return false;
    }
    for (size_t index = 0; index < size; index++) {
        sse_byte(&s_parser, data[index]);
    }
    return true;
}

esp_err_t mp_llm_parse_finish(mp_llm_result_t *result)
{
    if (!result || s_parser.result != result) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_parser.data_line && s_parser.data_started) {
        line_finish(&s_parser);
    }
    if (result->error[0]) {
        return ESP_FAIL;
    }
    size_t offset = 0;
    const mp_llm_call_t *call;
    while ((call = mp_llm_call_next(result, &offset))) {
        if (!call->item_id[0] || !call->name[0] || !call->call_id[0] ||
            !call->arguments[0]) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return result->call_count || (result->text[0] && result->message_id[0]) ?
           ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

const mp_llm_call_t *mp_llm_call_next(const mp_llm_result_t *result, size_t *offset)
{
    if (!result || !offset) {
        return NULL;
    }
    const mp_llm_call_t *next = NULL;
    size_t arena_offset = 0;
    while (arena_offset < result->arena_used) {
        const mp_llm_call_t *call =
            (const mp_llm_call_t *)(const_arena_data(result) + arena_offset);
        arena_offset += call->size;
        if (call->state == MP_LLM_CALL_READY && call->order > *offset &&
            (!next || call->order < next->order)) {
            next = call;
        }
    }
    if (next) {
        *offset = next->order;
    }
    return next;
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
        .buffer_size = 4096,
        .accepted_content_types = "text/event-stream,application/json"
    };
    mp_http_response_t response;
    esp_err_t error;
    bool retry;
    do {
        mp_wifi_wait(portMAX_DELAY);
        mp_llm_parse_begin(result);
        error = mp_http_session_stream(&s_session, &request, sse_chunk, result, &response);
        retry = mp_http_retryable(error) ||
                (error == ESP_OK && mp_http_status_retryable(response.status));
        if (retry) {
            mp_metrics_error("llm_retry",
                             error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    } while (retry);
    if (error != ESP_OK) {
        return error;
    }
    if (response.status != 200) {
        snprintf(result->error, sizeof(result->error), "Inference HTTP status %d.", response.status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return mp_llm_parse_finish(result);
}
