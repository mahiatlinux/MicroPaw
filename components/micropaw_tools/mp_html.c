#include "mp_html.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void append_char(mp_html_text_t *text, char value);
static void append_entity(mp_html_text_t *text);
static int entity_value(const char *entity);
void mp_html_text_init(mp_html_text_t *text, char *output, size_t size);
void mp_html_text_push(mp_html_text_t *text, char value);
void mp_html_text_finish(mp_html_text_t *text);

static void append_char(mp_html_text_t *text, char value)
{
    if (isspace((unsigned char)value)) {
        text->space = text->length > 0;
        return;
    }
    if (text->space && text->length + 1 < text->size) {
        text->output[text->length++] = ' ';
    }
    text->space = false;
    if (text->length + 1 < text->size) {
        text->output[text->length++] = value;
        text->output[text->length] = 0;
    }
}

static void append_entity(mp_html_text_t *text)
{
    text->entity[text->entity_length - 1] = 0;
    int value = entity_value(text->entity);
    if (value > 0 && value < 128) {
        append_char(text, (char)value);
    } else if (value == 160) {
        append_char(text, ' ');
    }
    text->entity_length = 0;
}

static int entity_value(const char *entity)
{
    if (strcmp(entity, "amp") == 0) {
        return '&';
    }
    if (strcmp(entity, "lt") == 0) {
        return '<';
    }
    if (strcmp(entity, "gt") == 0) {
        return '>';
    }
    if (strcmp(entity, "quot") == 0) {
        return '"';
    }
    if (strcmp(entity, "apos") == 0 || strcmp(entity, "#39") == 0 || strcmp(entity, "#x27") == 0) {
        return '\'';
    }
    if (strcmp(entity, "nbsp") == 0) {
        return 160;
    }
    if (entity[0] == '#' && (entity[1] == 'x' || entity[1] == 'X')) {
        return (int)strtol(entity + 2, NULL, 16);
    }
    if (entity[0] == '#') {
        return (int)strtol(entity + 1, NULL, 10);
    }
    return 0;
}

void mp_html_text_init(mp_html_text_t *text, char *output, size_t size)
{
    memset(text, 0, sizeof(*text));
    text->output = output;
    text->size = size;
    if (size) {
        output[0] = 0;
    }
}

void mp_html_text_push(mp_html_text_t *text, char value)
{
    if (text->entity_length) {
        if (value == ';') {
            append_entity(text);
        } else if (text->entity_length < sizeof(text->entity) &&
                   (isalnum((unsigned char)value) || value == '#' || value == 'x' || value == 'X')) {
            text->entity[text->entity_length++ - 1] = value;
        } else {
            text->entity_length = 0;
            append_char(text, value);
        }
    } else if (value == '&') {
        text->entity_length = 1;
    } else {
        append_char(text, value);
    }
}

void mp_html_text_finish(mp_html_text_t *text)
{
    text->entity_length = 0;
    text->space = false;
}
