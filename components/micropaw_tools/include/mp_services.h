#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    const char *to;
    const char *subject;
    const char *body;
} mp_email_t;

typedef struct {
    const char *summary;
    const char *start_rfc3339;
    const char *end_rfc3339;
} mp_calendar_event_t;

typedef struct {
    const char *name;
    esp_err_t (*send)(const mp_email_t *email, char *output, size_t size);
} mp_email_service_t;

typedef struct {
    const char *name;
    esp_err_t (*create)(const mp_calendar_event_t *event, char *output, size_t size);
} mp_calendar_service_t;

const mp_email_service_t *mp_email_service(void);
const mp_calendar_service_t *mp_calendar_service(void);
