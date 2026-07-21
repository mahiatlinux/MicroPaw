#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "mp_search.h"

extern const mp_search_provider_t mp_arxiv_provider;

esp_err_t mp_rss_read(const char *url, char *output, size_t size);
