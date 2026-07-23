#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void mp_metrics_register(const char *name, TaskHandle_t task);
void mp_metrics_request_begin(int64_t queued_us);
void mp_metrics_typing(int64_t queued_us);
void mp_metrics_progress(void);
void mp_metrics_inference(uint32_t elapsed_ms);
void mp_metrics_tool(const char *name, uint32_t elapsed_ms);
void mp_metrics_delivery(uint32_t elapsed_ms);
void mp_metrics_http(int status, size_t bytes, uint32_t connect_ms, uint32_t first_byte_ms,
                     uint32_t total_ms, bool reused);
void mp_metrics_format(char *output, size_t size);
void mp_metrics_log(void);
