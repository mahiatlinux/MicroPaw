#pragma once

#include <stddef.h>

#include "esp_err.h"

#define MP_SEARCH_MAX_RESULTS 5
#define MP_SEARCH_TITLE_LEN 160
#define MP_SEARCH_URL_LEN 512
#define MP_SEARCH_SNIPPET_LEN 320

typedef struct {
    char title[MP_SEARCH_TITLE_LEN];
    char url[MP_SEARCH_URL_LEN];
    char snippet[MP_SEARCH_SNIPPET_LEN];
} mp_search_result_t;

typedef struct {
    const char *name;
    esp_err_t (*search)(const char *query, mp_search_result_t *results, size_t *count);
} mp_search_provider_t;

const mp_search_provider_t *mp_search_provider(const char *name);
esp_err_t mp_search_run(const char *provider, const char *query, char *output, size_t size);
bool mp_search_url_allowed(const char *url);
void mp_search_allow_results(const mp_search_result_t *results, size_t count);
void mp_search_format_results(const mp_search_result_t *results, size_t count,
                              char *output, size_t size);
