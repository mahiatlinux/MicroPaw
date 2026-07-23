#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "mp_confirmation.h"
#include "mp_llm.h"
#include "unity.h"

EXT_RAM_BSS_ATTR static mp_llm_result_t s_result;
static char s_event[768];
static uint8_t s_repeat[1024];

static void feed(const char *text);
static void feed_bytes(const uint8_t *data, size_t size);
static void feed_bytewise(const char *text);
static void add_message(const char *id);
static void add_call(const char *item, const char *call, const char *name);
static void finish_call(const char *item, size_t arguments);
static const mp_llm_call_t *next_call(size_t *offset);

static void feed(const char *text)
{
    feed_bytes((const uint8_t *)text, strlen(text));
}

static void feed_bytes(const uint8_t *data, size_t size)
{
    TEST_ASSERT_TRUE(mp_llm_parse_chunk(&s_result, data, size));
}

static void feed_bytewise(const char *text)
{
    while (*text) {
        feed_bytes((const uint8_t *)text++, 1);
    }
}

static void add_message(const char *id)
{
    snprintf(s_event, sizeof(s_event),
             "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"message\",\"id\":\"%s\"}}\n",
             id);
    feed(s_event);
}

static void add_call(const char *item, const char *call, const char *name)
{
    snprintf(s_event, sizeof(s_event),
             "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"id\":\"%s\",\"call_id\":\"%s\",\"name\":\"%s\",\"arguments\":\"\"}}\n",
             item, call, name);
    feed(s_event);
}

static void finish_call(const char *item, size_t arguments)
{
    snprintf(s_event, sizeof(s_event),
             "data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"%s\",\"arguments\":\"",
             item);
    feed(s_event);
    memset(s_repeat, 'x', sizeof(s_repeat));
    while (arguments) {
        size_t size = arguments < sizeof(s_repeat) ? arguments : sizeof(s_repeat);
        feed_bytes(s_repeat, size);
        arguments -= size;
    }
    feed("\"}\n");
}

static const mp_llm_call_t *next_call(size_t *offset)
{
    const mp_llm_call_t *call = mp_llm_call_next(&s_result, offset);
    TEST_ASSERT_NOT_NULL(call);
    return call;
}

TEST_CASE("streamed message handles arbitrary chunks", "[micropaw][llm]")
{
    mp_llm_parse_begin(&s_result);
    feed_bytewise("data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"message\",\"id\":\"msg_1\"}}\n");
    feed_bytewise("data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello \\ud83c\\udf19\"}\n");
    TEST_ASSERT_EQUAL(ESP_OK, mp_llm_parse_finish(&s_result));
    TEST_ASSERT_EQUAL_STRING("msg_1", s_result.message_id);
    TEST_ASSERT_EQUAL_STRING("Hello \xf0\x9f\x8c\x99", s_result.text);
    TEST_ASSERT_EQUAL(0, s_result.call_count);
}

TEST_CASE("message and tool share one response", "[micropaw][llm]")
{
    mp_llm_parse_begin(&s_result);
    add_message("msg_progress");
    feed("data: {\"type\":\"response.output_text.delta\",\"delta\":\"Checking mail.\"}\n");
    add_call("fc_1", "call_1", "gmail_search");
    feed("data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_1\",\"arguments\":\"{\\\"query\\\":\\\"newer_than:1d\\\"}\"}\n");
    TEST_ASSERT_EQUAL(ESP_OK, mp_llm_parse_finish(&s_result));
    TEST_ASSERT_EQUAL_STRING("Checking mail.", s_result.text);
    size_t offset = 0;
    const mp_llm_call_t *call = next_call(&offset);
    TEST_ASSERT_EQUAL_STRING("gmail_search", call->name);
    TEST_ASSERT_EQUAL_STRING("{\"query\":\"newer_than:1d\"}", call->arguments);
    TEST_ASSERT_NULL(mp_llm_call_next(&s_result, &offset));
}

TEST_CASE("interleaved calls retain returned order", "[micropaw][llm]")
{
    mp_llm_parse_begin(&s_result);
    add_call("fc_1", "call_1", "first");
    add_call("fc_2", "call_2", "second");
    finish_call("fc_2", 2);
    finish_call("fc_1", 1);
    TEST_ASSERT_EQUAL(ESP_OK, mp_llm_parse_finish(&s_result));
    size_t offset = 0;
    TEST_ASSERT_EQUAL_STRING("first", next_call(&offset)->name);
    TEST_ASSERT_EQUAL_STRING("second", next_call(&offset)->name);
    TEST_ASSERT_NULL(mp_llm_call_next(&s_result, &offset));
}

TEST_CASE("maximum arguments fit the byte arena", "[micropaw][llm]")
{
    mp_llm_parse_begin(&s_result);
    add_call("fc_max", "call_max", "maximum");
    finish_call("fc_max", MP_TOOL_ARGS_LEN);
    TEST_ASSERT_EQUAL(ESP_OK, mp_llm_parse_finish(&s_result));
    size_t offset = 0;
    TEST_ASSERT_EQUAL(MP_TOOL_ARGS_LEN, next_call(&offset)->arguments_length);
}

TEST_CASE("byte arena overflow fails the response", "[micropaw][llm]")
{
    mp_llm_parse_begin(&s_result);
    add_call("fc_over", "call_over", "overflow");
    finish_call("fc_over", MP_LLM_CALL_ARENA_LEN);
    TEST_ASSERT_EQUAL(ESP_FAIL, mp_llm_parse_finish(&s_result));
    TEST_ASSERT_NOT_EQUAL(0, s_result.error[0]);
}

TEST_CASE("provider failure is retained", "[micropaw][llm]")
{
    mp_llm_parse_begin(&s_result);
    feed("data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"message\":\"rate limited\"}}}\n");
    TEST_ASSERT_EQUAL(ESP_FAIL, mp_llm_parse_finish(&s_result));
    TEST_ASSERT_EQUAL_STRING("rate limited", s_result.error);
}

TEST_CASE("parallel permissions retain separate ids", "[micropaw][permission]")
{
    char tool[MP_TOOL_NAME_LEN];
    char arguments[64];
    uint32_t first;
    uint32_t second;
    mp_confirmation_reset();
    TEST_ASSERT_EQUAL(ESP_OK,
                      mp_confirmation_request("123", "first", "{\"id\":1}", &first));
    TEST_ASSERT_EQUAL(ESP_OK,
                      mp_confirmation_request("123", "second", "{\"id\":2}", &second));
    TEST_ASSERT_NOT_EQUAL(first, second);
    TEST_ASSERT_EQUAL(
        ESP_OK, mp_confirmation_take("123", second, tool, sizeof(tool),
                                     arguments, sizeof(arguments)));
    TEST_ASSERT_EQUAL_STRING("second", tool);
    TEST_ASSERT_EQUAL_STRING("{\"id\":2}", arguments);
    TEST_ASSERT_EQUAL(
        ESP_OK, mp_confirmation_take("123", first, tool, sizeof(tool),
                                     arguments, sizeof(arguments)));
    TEST_ASSERT_EQUAL_STRING("first", tool);
    TEST_ASSERT_EQUAL_STRING("{\"id\":1}", arguments);
}
