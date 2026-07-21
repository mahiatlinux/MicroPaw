#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *output;
    size_t size;
    size_t length;
    char entity[12];
    size_t entity_length;
    bool space;
} mp_html_text_t;

void mp_html_text_init(mp_html_text_t *text, char *output, size_t size);
void mp_html_text_push(mp_html_text_t *text, char value);
void mp_html_text_finish(mp_html_text_t *text);
