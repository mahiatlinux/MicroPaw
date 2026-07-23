#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    MP_DISPLAY_IDLE,
    MP_DISPLAY_THINKING,
    MP_DISPLAY_RESPONDING
} mp_display_activity_t;

typedef enum {
    MP_DISPLAY_HAPPY,
    MP_DISPLAY_SAD,
    MP_DISPLAY_SURPRISED,
    MP_DISPLAY_SLEEPY
} mp_display_mood_t;

esp_err_t mp_display_start(void);
void mp_display_agent_begin(void);
void mp_display_agent_end(void);
void mp_display_response_begin(void);
void mp_display_response_end(void);
void mp_display_filter_text(char *text);
bool mp_display_enabled(void);
