#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t mp_wifi_start(void);
bool mp_wifi_wait(TickType_t timeout);
bool mp_wifi_connected(void);
