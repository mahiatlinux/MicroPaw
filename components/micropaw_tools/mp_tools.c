#include "mp_tools.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "mp_confirmation.h"
#include "mp_feed.h"
#include "mp_json.h"
#include "mp_memory.h"
#include "mp_metrics.h"
#include "mp_scheduler.h"
#include "mp_search.h"
#include "mp_services.h"
#include "sdkconfig.h"

#if CONFIG_MICROPAW_WIKIPEDIA
#define MP_PROVIDER_WIKIPEDIA ",\"wikipedia\""
#else
#define MP_PROVIDER_WIKIPEDIA ""
#endif

#if CONFIG_MICROPAW_ARXIV
#define MP_PROVIDER_ARXIV ",\"arxiv\""
#else
#define MP_PROVIDER_ARXIV ""
#endif

typedef esp_err_t (*tool_fn)(const char *arguments, const mp_tool_context_t *context,
                             char *output, size_t size);

typedef struct {
    const char *name;
    const char *schema;
    bool confirmation;
    tool_fn execute;
} tool_t;

EXT_RAM_BSS_ATTR static char s_arg1[2048];
#if CONFIG_MICROPAW_GMAIL || CONFIG_MICROPAW_CALENDAR
EXT_RAM_BSS_ATTR static char s_arg2[512];
#endif
#if CONFIG_MICROPAW_WEB_SEARCH || CONFIG_MICROPAW_GMAIL || CONFIG_MICROPAW_CALENDAR
EXT_RAM_BSS_ATTR static char s_arg3[256];
#endif
#if CONFIG_MICROPAW_CALENDAR
EXT_RAM_BSS_ATTR static char s_arg4[128];
#endif

static bool json_string(const char *json, const char *name, char *output, size_t size);
static bool json_uint(const char *json, const char *name, uint32_t *value);
static esp_err_t memory_save(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t memory_list(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t schedule_add(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t schedule_list(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t schedule_delete(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t time_now(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t diagnostics(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t web_search(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t web_fetch(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
static esp_err_t rss_read(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
#if CONFIG_MICROPAW_GMAIL
static esp_err_t email_send(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
#endif
#if CONFIG_MICROPAW_CALENDAR
static esp_err_t calendar_create(const char *arguments, const mp_tool_context_t *context, char *output, size_t size);
#endif
esp_err_t mp_tools_init(void);
bool mp_tools_json(char *output, size_t size);
esp_err_t mp_tools_execute(const char *name, const char *arguments,
                           const mp_tool_context_t *context, bool confirmed,
                           char *output, size_t size);

static const tool_t s_tools[] = {
    {"memory_save", "{\"type\":\"function\",\"name\":\"memory_save\",\"description\":\"Save one durable fact for later conversations.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"note\":{\"type\":\"string\"}},\"required\":[\"note\"],\"additionalProperties\":false}}", false, memory_save},
    {"memory_list", "{\"type\":\"function\",\"name\":\"memory_list\",\"description\":\"Read durable facts.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}}", false, memory_list},
    {"schedule_add", "{\"type\":\"function\",\"name\":\"schedule_add\",\"description\":\"Schedule a future assistant turn. repeat_seconds is zero for a one-time job.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"delay_seconds\":{\"type\":\"integer\"},\"repeat_seconds\":{\"type\":\"integer\"}},\"required\":[\"prompt\",\"delay_seconds\",\"repeat_seconds\"],\"additionalProperties\":false}}", false, schedule_add},
    {"schedule_list", "{\"type\":\"function\",\"name\":\"schedule_list\",\"description\":\"List scheduled jobs.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}}", false, schedule_list},
    {"schedule_delete", "{\"type\":\"function\",\"name\":\"schedule_delete\",\"description\":\"Delete a scheduled job by ID.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"],\"additionalProperties\":false}}", false, schedule_delete},
    {"time_now", "{\"type\":\"function\",\"name\":\"time_now\",\"description\":\"Read the device local date and time.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}}", false, time_now},
    {"diagnostics", "{\"type\":\"function\",\"name\":\"diagnostics\",\"description\":\"Read heap and task stack measurements.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}}", false, diagnostics},
#if CONFIG_MICROPAW_WEB_SEARCH
    {"web_search", "{\"type\":\"function\",\"name\":\"web_search\",\"description\":\"Search through DuckDuckGo Lite or an official specialised source. Returned page URLs may be passed to web_fetch.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"provider\":{\"type\":\"string\",\"enum\":[\"duckduckgo\"" MP_PROVIDER_WIKIPEDIA MP_PROVIDER_ARXIV "]}},\"required\":[\"query\",\"provider\"],\"additionalProperties\":false}}", false, web_search},
#if CONFIG_MICROPAW_WEB_FETCH
    {"web_fetch", "{\"type\":\"function\",\"name\":\"web_fetch\",\"description\":\"Fetch one HTTPS URL from the most recent search results and return bounded visible text.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"],\"additionalProperties\":false}}", false, web_fetch},
#endif
#endif
#if CONFIG_MICROPAW_RSS
    {"rss_read", "{\"type\":\"function\",\"name\":\"rss_read\",\"description\":\"Read the first five items from a public HTTPS RSS or Atom feed.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"],\"additionalProperties\":false}}", false, rss_read},
#endif
#if CONFIG_MICROPAW_GMAIL
    {"email_send", "{\"type\":\"function\",\"name\":\"email_send\",\"description\":\"Send a plain-text email through Gmail after user confirmation.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"to\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},\"required\":[\"to\",\"subject\",\"body\"],\"additionalProperties\":false}}", true, email_send},
#endif
#if CONFIG_MICROPAW_CALENDAR
    {"calendar_create", "{\"type\":\"function\",\"name\":\"calendar_create\",\"description\":\"Create a Google Calendar event after user confirmation.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"summary\":{\"type\":\"string\"},\"start_rfc3339\":{\"type\":\"string\"},\"end_rfc3339\":{\"type\":\"string\"}},\"required\":[\"summary\",\"start_rfc3339\",\"end_rfc3339\"],\"additionalProperties\":false}}", true, calendar_create},
#endif
};

static bool json_string(const char *json, const char *name, char *output, size_t size)
{
    return mp_json_get_string(json, strlen(json), name, output, size);
}

static bool json_uint(const char *json, const char *name, uint32_t *value)
{
    int64_t parsed;
    if (!mp_json_get_int64(json, strlen(json), name, &parsed) || parsed < 0 || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static esp_err_t memory_save(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)context;
    esp_err_t error = json_string(arguments, "note", s_arg1, sizeof(s_arg1)) ?
                      mp_memory_save(s_arg1) : ESP_ERR_INVALID_ARG;
    if (error == ESP_OK) {
        strlcpy(output, "Memory saved.", size);
    }
    return error;
}

static esp_err_t memory_list(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)arguments;
    (void)context;
    mp_memory_format(output, size);
    if (!output[0]) {
        strlcpy(output, "No saved memory.", size);
    }
    return ESP_OK;
}

static esp_err_t schedule_add(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    uint32_t delay;
    uint32_t repeat;
    uint32_t id;
    if (!json_string(arguments, "prompt", s_arg1, sizeof(s_arg1)) ||
        !json_uint(arguments, "delay_seconds", &delay) ||
        !json_uint(arguments, "repeat_seconds", &repeat) || delay == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = mp_scheduler_add(context->chat_id, s_arg1, delay, repeat, &id);
    if (error == ESP_OK) {
        snprintf(output, size, "Scheduled job %lu.", (unsigned long)id);
    }
    return error;
}

static esp_err_t schedule_list(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)arguments;
    (void)context;
    mp_scheduler_format(output, size);
    return ESP_OK;
}

static esp_err_t schedule_delete(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)context;
    uint32_t id;
    if (!json_uint(arguments, "id", &id)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = mp_scheduler_delete(id);
    if (error == ESP_OK) {
        strlcpy(output, "Scheduled job deleted.", size);
    }
    return error;
}

static esp_err_t time_now(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)arguments;
    (void)context;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return strftime(output, size, "%Y-%m-%dT%H:%M:%S%z", &local) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t diagnostics(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)arguments;
    (void)context;
    mp_metrics_format(output, size);
    return ESP_OK;
}

static esp_err_t web_search(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)context;
    return json_string(arguments, "query", s_arg1, sizeof(s_arg1)) &&
           json_string(arguments, "provider", s_arg3, sizeof(s_arg3)) ?
           mp_search_run(s_arg3, s_arg1, output, size) : ESP_ERR_INVALID_ARG;
}

static esp_err_t web_fetch(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)context;
    return json_string(arguments, "url", s_arg1, sizeof(s_arg1)) ?
           mp_page_fetch(s_arg1, output, size) : ESP_ERR_INVALID_ARG;
}

static esp_err_t rss_read(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)context;
    return json_string(arguments, "url", s_arg1, sizeof(s_arg1)) ?
           mp_rss_read(s_arg1, output, size) : ESP_ERR_INVALID_ARG;
}

#if CONFIG_MICROPAW_GMAIL
static esp_err_t email_send(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)context;
    if (!json_string(arguments, "to", s_arg3, sizeof(s_arg3)) ||
        !json_string(arguments, "subject", s_arg2, sizeof(s_arg2)) ||
        !json_string(arguments, "body", s_arg1, sizeof(s_arg1))) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_email_t email = {
        .to = s_arg3,
        .subject = s_arg2,
        .body = s_arg1
    };
    return mp_email_service()->send(&email, output, size);
}
#endif

#if CONFIG_MICROPAW_CALENDAR
static esp_err_t calendar_create(const char *arguments, const mp_tool_context_t *context, char *output, size_t size)
{
    (void)context;
    if (!json_string(arguments, "summary", s_arg2, sizeof(s_arg2)) ||
        !json_string(arguments, "start_rfc3339", s_arg3, sizeof(s_arg3)) ||
        !json_string(arguments, "end_rfc3339", s_arg4, sizeof(s_arg4))) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_calendar_event_t event = {
        .summary = s_arg2,
        .start_rfc3339 = s_arg3,
        .end_rfc3339 = s_arg4
    };
    return mp_calendar_service()->create(&event, output, size);
}
#endif

esp_err_t mp_tools_init(void)
{
    return ESP_OK;
}

bool mp_tools_json(char *output, size_t size)
{
    mp_writer_t writer;
    mp_writer_init(&writer, output, size);
    mp_writer_char(&writer, '[');
    for (size_t index = 0; index < sizeof(s_tools) / sizeof(s_tools[0]); index++) {
        if (index) {
            mp_writer_char(&writer, ',');
        }
        mp_writer_raw(&writer, s_tools[index].schema);
    }
    mp_writer_char(&writer, ']');
    return writer.valid;
}

esp_err_t mp_tools_execute(const char *name, const char *arguments,
                           const mp_tool_context_t *context, bool confirmed,
                           char *output, size_t size)
{
    if (!name || !arguments || !context || !output || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const tool_t *tool = NULL;
    for (size_t index = 0; index < sizeof(s_tools) / sizeof(s_tools[0]); index++) {
        if (strcmp(name, s_tools[index].name) == 0) {
            tool = &s_tools[index];
            break;
        }
    }
    if (!tool) {
        return ESP_ERR_NOT_FOUND;
    }
    if (tool->confirmation && !confirmed) {
        uint32_t id;
        esp_err_t error = mp_confirmation_request(context->chat_id, name, arguments, &id);
        if (error == ESP_OK) {
            snprintf(output, size, "Confirmation required. Send /confirm %lu within five minutes.",
                     (unsigned long)id);
        }
        return error;
    }
    if (arguments[0] != '{') {
        return ESP_ERR_INVALID_ARG;
    }
    return tool->execute(arguments, context, output, size);
}
