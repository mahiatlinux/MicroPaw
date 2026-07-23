#include "mp_metrics.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mp_config.h"
#include "mp_context.h"
#include "mp_types.h"

typedef struct {
    const char *name;
    TaskHandle_t task;
} task_metric_t;

typedef struct {
    int64_t started_us;
    uint32_t queue_ms;
    uint32_t typing_ms;
    uint32_t progress_ms;
    uint32_t inference_ms;
    uint32_t inference_rounds;
    uint32_t tool_ms;
    uint32_t tool_count;
    uint32_t delivery_ms;
    char slowest_tool[MP_TOOL_NAME_LEN];
    uint32_t slowest_tool_ms;
    int http_status;
    size_t http_bytes;
    uint32_t http_connect_ms;
    uint32_t http_first_byte_ms;
    uint32_t http_total_ms;
    bool http_reused;
    char error_source[24];
    esp_err_t error;
    uint32_t error_count;
} metrics_t;

static const char *TAG = "metrics";
static task_metric_t s_tasks[8];
static size_t s_task_count;
static metrics_t s_metrics;

void mp_metrics_register(const char *name, TaskHandle_t task);
void mp_metrics_request_begin(int64_t queued_us);
void mp_metrics_typing(int64_t queued_us);
void mp_metrics_progress(void);
void mp_metrics_inference(uint32_t elapsed_ms);
void mp_metrics_tool(const char *name, uint32_t elapsed_ms);
void mp_metrics_delivery(uint32_t elapsed_ms);
void mp_metrics_http(int status, size_t bytes, uint32_t connect_ms, uint32_t first_byte_ms,
                     uint32_t total_ms, bool reused);
void mp_metrics_error(const char *source, esp_err_t error);
void mp_metrics_format(char *output, size_t size);
void mp_metrics_log(void);

void mp_metrics_register(const char *name, TaskHandle_t task)
{
    if (name && task && s_task_count < sizeof(s_tasks) / sizeof(s_tasks[0])) {
        s_tasks[s_task_count++] = (task_metric_t){name, task};
    }
}

void mp_metrics_request_begin(int64_t queued_us)
{
    uint32_t typing_ms = s_metrics.typing_ms;
    memset(&s_metrics.queue_ms, 0,
           offsetof(metrics_t, http_status) - offsetof(metrics_t, queue_ms));
    s_metrics.started_us = queued_us;
    s_metrics.queue_ms = (uint32_t)((esp_timer_get_time() - queued_us) / 1000);
    s_metrics.typing_ms = typing_ms;
}

void mp_metrics_typing(int64_t queued_us)
{
    s_metrics.typing_ms = (uint32_t)((esp_timer_get_time() - queued_us) / 1000);
}

void mp_metrics_progress(void)
{
    if (!s_metrics.progress_ms && s_metrics.started_us) {
        s_metrics.progress_ms = (uint32_t)((esp_timer_get_time() - s_metrics.started_us) / 1000);
    }
}

void mp_metrics_inference(uint32_t elapsed_ms)
{
    s_metrics.inference_ms += elapsed_ms;
    s_metrics.inference_rounds++;
}

void mp_metrics_tool(const char *name, uint32_t elapsed_ms)
{
    s_metrics.tool_ms += elapsed_ms;
    s_metrics.tool_count++;
    if (elapsed_ms >= s_metrics.slowest_tool_ms) {
        s_metrics.slowest_tool_ms = elapsed_ms;
        strlcpy(s_metrics.slowest_tool, name, sizeof(s_metrics.slowest_tool));
    }
}

void mp_metrics_delivery(uint32_t elapsed_ms)
{
    s_metrics.delivery_ms = elapsed_ms;
}

void mp_metrics_http(int status, size_t bytes, uint32_t connect_ms, uint32_t first_byte_ms,
                     uint32_t total_ms, bool reused)
{
    s_metrics.http_status = status;
    s_metrics.http_bytes = bytes;
    s_metrics.http_connect_ms = connect_ms;
    s_metrics.http_first_byte_ms = first_byte_ms;
    s_metrics.http_total_ms = total_ms;
    s_metrics.http_reused = reused;
}

void mp_metrics_error(const char *source, esp_err_t error)
{
    strlcpy(s_metrics.error_source, source, sizeof(s_metrics.error_source));
    s_metrics.error = error;
    s_metrics.error_count++;
}

void mp_metrics_format(char *output, size_t size)
{
    if (size == 0) {
        return;
    }
    int used = snprintf(
        output, size,
        "heap_free=%lu heap_min=%lu heap_largest=%lu\n"
        "internal_free=%lu internal_min=%lu internal_largest=%lu\n"
        "psram_free=%lu psram_min=%lu psram_largest=%lu\n"
        "text_capacity=%u agent_request_capacity=%u tool_trace_capacity=%u tool_args_capacity=%u tool_result_capacity=%u llm_stream_capacity=%u llm_output_tokens=%s\n"
        "google_response_capacity=%u page_download_capacity=%u search_response_capacity=%u incoming_text_capacity=%u email_body_capacity=%u decoded_email_capacity=%u page_records=%u\n"
        "memory_slots=%u memory_entry_capacity=%u scheduled_slots=%u scheduled_entry_capacity=%u\n"
        "last_request queue_ms=%lu typing_ms=%lu progress_ms=%lu inference_ms=%lu rounds=%lu tools=%lu tool_ms=%lu delivery_ms=%lu\n"
        "slowest_tool=%s slowest_tool_ms=%lu\n"
        "context_bytes=%lu compactions=%lu compaction_ms=%lu compaction_failures=%lu\n"
        "last_http status=%d bytes=%lu connect_ms=%lu first_byte_ms=%lu total_ms=%lu reused=%s\n"
        "last_error source=%s code=%s count=%lu\n",
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)MP_REPLY_LEN, (unsigned)MP_AGENT_REQUEST_LEN, (unsigned)MP_TOOL_TRACE_LEN,
        (unsigned)MP_TOOL_ARGS_LEN, (unsigned)MP_TOOL_RESULT_LEN,
        (unsigned)CONFIG_MICROPAW_LLM_STREAM_LIMIT, mp_config_get()->llm_max_output_tokens,
        (unsigned)CONFIG_MICROPAW_WORK_TEXT_BYTES,
        (unsigned)CONFIG_MICROPAW_PAGE_DOWNLOAD_LIMIT,
        (unsigned)CONFIG_MICROPAW_SEARCH_DOWNLOAD_LIMIT,
        (unsigned)(MP_MESSAGE_LEN - 1), (unsigned)(MP_REPLY_LEN - 1),
        (unsigned)MP_TOOL_RESULT_LEN, 20U,
        (unsigned)MP_MEMORY_SLOTS, (unsigned)(MP_MEMORY_TEXT_LEN - 1),
        (unsigned)MP_SCHEDULE_SLOTS, (unsigned)(MP_SCHEDULE_TEXT_LEN - 1),
        (unsigned long)s_metrics.queue_ms, (unsigned long)s_metrics.typing_ms,
        (unsigned long)s_metrics.progress_ms, (unsigned long)s_metrics.inference_ms,
        (unsigned long)s_metrics.inference_rounds, (unsigned long)s_metrics.tool_count,
        (unsigned long)s_metrics.tool_ms, (unsigned long)s_metrics.delivery_ms,
        s_metrics.slowest_tool[0] ? s_metrics.slowest_tool : "none",
        (unsigned long)s_metrics.slowest_tool_ms, (unsigned long)mp_context_stored_bytes(),
        (unsigned long)mp_context_compaction_count(),
        (unsigned long)mp_context_last_compaction_ms(),
        (unsigned long)mp_context_compaction_failures(), s_metrics.http_status,
        (unsigned long)s_metrics.http_bytes, (unsigned long)s_metrics.http_connect_ms,
        (unsigned long)s_metrics.http_first_byte_ms, (unsigned long)s_metrics.http_total_ms,
        s_metrics.http_reused ? "yes" : "no",
        s_metrics.error_source[0] ? s_metrics.error_source : "none",
        esp_err_to_name(s_metrics.error), (unsigned long)s_metrics.error_count);
    if (used < 0 || (size_t)used >= size) {
        output[size - 1] = 0;
        return;
    }
    for (size_t index = 0; index < s_task_count; index++) {
        size_t offset = strlen(output);
        if (offset < size) {
            snprintf(output + offset, size - offset, "stack_%s_min_free=%lu\n", s_tasks[index].name,
                     (unsigned long)uxTaskGetStackHighWaterMark(s_tasks[index].task));
        }
    }
}

void mp_metrics_log(void)
{
    char output[1536];
    mp_metrics_format(output, sizeof(output));
    ESP_LOGI(TAG, "%s", output);
}
