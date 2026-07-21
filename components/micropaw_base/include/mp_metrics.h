#pragma once

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void mp_metrics_register(const char *name, TaskHandle_t task);
void mp_metrics_format(char *output, size_t size);
void mp_metrics_log(void);
