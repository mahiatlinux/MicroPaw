#include "mp_html.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void append_char(mp_html_text_t *text, char value);
static void append_entity(mp_html_text_t *text);
static int entity_value(const char *entity);
static void parser_tag(mp_html_parser_t *parser);
void mp_html_text_init(mp_html_text_t *text, char *output, size_t size);
void mp_html_text_push(mp_html_text_t *text, char value);
void mp_html_text_finish(mp_html_text_t *text);
void mp_html_parser_init(mp_html_parser_t *parser, char *output, size_t size);
bool mp_html_parser_push(mp_html_parser_t *parser, char value);
void mp_html_parser_finish(mp_html_parser_t *parser);

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

static void parser_tag(mp_html_parser_t *parser)
{
    parser->tag[parser->tag_length] = 0;
    if (strcmp(parser->tag, "script") == 0 || strcmp(parser->tag, "style") == 0 ||
        strcmp(parser->tag, "noscript") == 0) {
        parser->suppress = true;
    } else if (strcmp(parser->tag, "/script") == 0 || strcmp(parser->tag, "/style") == 0 ||
               strcmp(parser->tag, "/noscript") == 0) {
        parser->suppress = false;
    } else if (!parser->suppress && (parser->tag[0] == '/' || strcmp(parser->tag, "br") == 0 ||
               strcmp(parser->tag, "p") == 0 || strcmp(parser->tag, "li") == 0)) {
        mp_html_text_push(&parser->text, ' ');
    }
    parser->tag_length = 0;
}

void mp_html_parser_init(mp_html_parser_t *parser, char *output, size_t size)
{
    memset(parser, 0, sizeof(*parser));
    mp_html_text_init(&parser->text, output, size);
}

bool mp_html_parser_push(mp_html_parser_t *parser, char value)
{
    if (parser->in_tag) {
        if (value == '>') {
            parser->in_tag = false;
            parser_tag(parser);
        } else if (isspace((unsigned char)value)) {
            parser->tag_done = true;
        } else if (!parser->tag_done && parser->tag_length + 1 < sizeof(parser->tag) &&
                   (isalpha((unsigned char)value) || (value == '/' && parser->tag_length == 0))) {
            parser->tag[parser->tag_length++] = (char)tolower((unsigned char)value);
        }
    } else if (value == '<') {
        parser->in_tag = true;
        parser->tag_length = 0;
        parser->tag_done = false;
    } else if (!parser->suppress) {
        mp_html_text_push(&parser->text, value);
    }
    return parser->text.length + 1 < parser->text.size;
}

void mp_html_parser_finish(mp_html_parser_t *parser)
{
    mp_html_text_finish(&parser->text);
}
