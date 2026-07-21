#include "mp_metrics.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

typedef struct {
    const char *name;
    TaskHandle_t task;
} task_metric_t;

static const char *TAG = "metrics";
static task_metric_t s_tasks[5];
static size_t s_task_count;

void mp_metrics_register(const char *name, TaskHandle_t task);
void mp_metrics_format(char *output, size_t size);
void mp_metrics_log(void);

void mp_metrics_register(const char *name, TaskHandle_t task)
{
    if (name && task && s_task_count < sizeof(s_tasks) / sizeof(s_tasks[0])) {
        s_tasks[s_task_count++] = (task_metric_t){name, task};
    }
}

void mp_metrics_format(char *output, size_t size)
{
    if (size == 0) {
        return;
    }
    int used = snprintf(output, size,
                        "heap_free=%lu heap_min=%lu heap_largest=%lu\n"
                        "internal_free=%lu internal_min=%lu internal_largest=%lu\n"
                        "psram_free=%lu psram_min=%lu psram_largest=%lu\n",
                        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
    char output[768];
    mp_metrics_format(output, sizeof(output));
    ESP_LOGI(TAG, "%s", output);
}
