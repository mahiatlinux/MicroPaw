#pragma once

#include <stddef.h>
#include <stdint.h>

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
    esp_err_t (*search)(const char *query, const char *page_token, uint32_t page_size,
                        char *output, size_t size);
    esp_err_t (*get)(const char *id, char *output, size_t size);
    esp_err_t (*modify)(const char *id, const char *action, char *output, size_t size);
    esp_err_t (*trash)(const char *id, char *output, size_t size);
    esp_err_t (*untrash)(const char *id, char *output, size_t size);
} mp_email_service_t;

typedef struct {
    const char *name;
    esp_err_t (*create)(const mp_calendar_event_t *event, char *output, size_t size);
    esp_err_t (*list)(const char *query, const char *time_min, const char *time_max,
                      const char *page_token, uint32_t page_size, char *output, size_t size);
    esp_err_t (*get)(const char *id, char *output, size_t size);
    esp_err_t (*update)(const char *id, const mp_calendar_event_t *event,
                        char *output, size_t size);
    esp_err_t (*remove)(const char *id, const char *send_updates, char *output, size_t size);
} mp_calendar_service_t;

const mp_email_service_t *mp_email_service(void);
const mp_calendar_service_t *mp_calendar_service(void);
