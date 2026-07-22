#include "mp_services.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "mp_google.h"
#include "mp_html.h"
#include "mp_json.h"
#include "mp_types.h"

#define GMAIL_PAGE_SIZE 20

typedef struct {
    char id[32];
} gmail_id_t;

typedef struct {
    const char *data;
    size_t data_length;
    char attachment_id[256];
    bool html;
} gmail_part_t;

typedef struct {
    const char *header;
    size_t header_size;
    const char *body;
} email_write_context_t;

typedef struct {
    esp_http_client_handle_t client;
    size_t output_size;
    uint8_t tail[3];
    size_t tail_size;
} base64_stream_t;

EXT_RAM_BSS_ATTR static char s_url[4096];
EXT_RAM_BSS_ATTR static char s_encoded[2048];
EXT_RAM_BSS_ATTR static char s_email_header[1024];
EXT_RAM_BSS_ATTR static char s_encode_buffer[2048];
EXT_RAM_BSS_ATTR static char s_decoded[MP_TOOL_RESULT_LEN];
EXT_RAM_BSS_ATTR static gmail_id_t s_ids[GMAIL_PAGE_SIZE];
EXT_RAM_BSS_ATTR static char s_next_page[1024];

static bool gmail_header(const char *payload, size_t length, const char *wanted,
                         char *output, size_t size);
static size_t gmail_ids(const char *response);
static esp_err_t gmail_metadata(const char *id, uint32_t number, mp_writer_t *writer,
                                char *output, size_t size);
static int base64_value(char value);
static esp_err_t base64_decode(const char *input, size_t length, char *output, size_t size);
static bool gmail_find_part(const char *part, size_t length, const char *mime,
                            gmail_part_t *found, unsigned depth);
static esp_err_t gmail_attachment(const char *message_id, const char *attachment_id,
                                  char *output, size_t size);
static void gmail_labels(const char *response, mp_writer_t *writer);
static esp_err_t gmail_send(const mp_email_t *email, char *output, size_t size);
static esp_err_t gmail_search(const char *query, const char *page_token, uint32_t page_size,
                              char *output, size_t size);
static esp_err_t gmail_get(const char *id, char *output, size_t size);
static esp_err_t gmail_modify(const char *id, const char *action, char *output, size_t size);
static esp_err_t gmail_trash(const char *id, char *output, size_t size);
static esp_err_t gmail_untrash(const char *id, char *output, size_t size);
static esp_err_t gmail_move(const char *id, const char *operation, char *output, size_t size);
static esp_err_t write_all(esp_http_client_handle_t client, const char *data, size_t size);
static esp_err_t base64_flush(base64_stream_t *stream);
static esp_err_t base64_emit(base64_stream_t *stream, const uint8_t *input, size_t size);
static esp_err_t base64_feed(base64_stream_t *stream, const uint8_t *input, size_t size);
static esp_err_t base64_finish(base64_stream_t *stream);
static esp_err_t email_write(esp_http_client_handle_t client, void *context);
static size_t base64url_size(size_t size);
const mp_email_service_t *mp_email_service(void);

static const mp_email_service_t s_service = {
    "gmail", gmail_send, gmail_search, gmail_get, gmail_modify, gmail_trash, gmail_untrash
};

static bool gmail_header(const char *payload, size_t length, const char *wanted,
                         char *output, size_t size)
{
    const char *headers;
    size_t headers_length;
    if (!mp_json_get_slice(payload, length, "headers", &headers, &headers_length)) {
        output[0] = 0;
        return false;
    }
    size_t offset = 0;
    const char *header;
    size_t header_length;
    char name[32];
    while (mp_json_next(headers, headers_length, &offset, &header, &header_length)) {
        if (mp_json_get_string(header, header_length, "name", name, sizeof(name)) &&
            strcasecmp(name, wanted) == 0) {
            return mp_json_get_string(header, header_length, "value", output, size);
        }
    }
    output[0] = 0;
    return false;
}

static size_t gmail_ids(const char *response)
{
    s_next_page[0] = 0;
    size_t length = strlen(response);
    mp_json_get_string(response, length, "nextPageToken", s_next_page, sizeof(s_next_page));
    const char *messages;
    size_t messages_length;
    if (!mp_json_get_slice(response, length, "messages", &messages, &messages_length)) {
        return 0;
    }
    size_t count = 0;
    size_t offset = 0;
    const char *message;
    size_t message_length;
    while (count < GMAIL_PAGE_SIZE &&
           mp_json_next(messages, messages_length, &offset, &message, &message_length)) {
        if (mp_json_get_string(message, message_length, "id", s_ids[count].id,
                               sizeof(s_ids[count].id))) {
            count++;
        }
    }
    return count;
}

static esp_err_t gmail_metadata(const char *id, uint32_t number, mp_writer_t *writer,
                                char *output, size_t size)
{
    snprintf(s_url, sizeof(s_url),
             "https://gmail.googleapis.com/gmail/v1/users/me/messages/%s?format=metadata&metadataHeaders=From&metadataHeaders=Subject&metadataHeaders=Date",
             id);
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_GET, s_url, NULL, 0, NULL, NULL, 20000,
                                         "application/json", &response, output, size);
    if (result != ESP_OK) {
        return result;
    }
    const char *json = mp_google_response();
    size_t length = strlen(json);
    const char *payload;
    size_t payload_length;
    char from[256] = "";
    char subject[512] = "";
    char date[128] = "";
    char snippet[512] = "";
    if (mp_json_get_slice(json, length, "payload", &payload, &payload_length)) {
        gmail_header(payload, payload_length, "From", from, sizeof(from));
        gmail_header(payload, payload_length, "Subject", subject, sizeof(subject));
        gmail_header(payload, payload_length, "Date", date, sizeof(date));
    }
    mp_json_get_string(json, length, "snippet", snippet, sizeof(snippet));
    mp_writer_format(writer, "%lu. %s\nID: %s\nFrom: %s\nDate: %s\n%s\n\n",
                     (unsigned long)number, subject[0] ? subject : "(no subject)", id,
                     from[0] ? from : "(unknown)", date[0] ? date : "(unknown)", snippet);
    return writer->valid ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static int base64_value(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '-' || value == '+') {
        return 62;
    }
    if (value == '_' || value == '/') {
        return 63;
    }
    return -1;
}

static esp_err_t base64_decode(const char *input, size_t length, char *output, size_t size)
{
    if (!input || !output || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t bits = 0;
    unsigned count = 0;
    size_t used = 0;
    for (size_t index = 0; index < length; index++) {
        if (input[index] == '=') {
            break;
        }
        int value = base64_value(input[index]);
        if (value < 0) {
            continue;
        }
        bits = (bits << 6) | (uint32_t)value;
        count += 6;
        if (count >= 8) {
            count -= 8;
            if (used + 1 >= size) {
                output[used] = 0;
                return ESP_ERR_INVALID_SIZE;
            }
            output[used++] = (char)(bits >> count);
            bits &= count ? (1U << count) - 1U : 0;
        }
    }
    output[used] = 0;
    return ESP_OK;
}

static bool gmail_find_part(const char *part, size_t length, const char *mime,
                            gmail_part_t *found, unsigned depth)
{
    if (depth > 8) {
        return false;
    }
    char type[64];
    if (mp_json_get_string(part, length, "mimeType", type, sizeof(type)) &&
        strcasecmp(type, mime) == 0) {
        const char *body;
        size_t body_length;
        if (mp_json_get_slice(part, length, "body", &body, &body_length)) {
            if (mp_json_get_slice(body, body_length, "data", &found->data, &found->data_length)) {
                found->html = strcasecmp(mime, "text/html") == 0;
                return true;
            }
            if (mp_json_get_string(body, body_length, "attachmentId", found->attachment_id,
                                   sizeof(found->attachment_id))) {
                found->html = strcasecmp(mime, "text/html") == 0;
                return true;
            }
        }
    }
    const char *parts;
    size_t parts_length;
    if (!mp_json_get_slice(part, length, "parts", &parts, &parts_length)) {
        return false;
    }
    size_t offset = 0;
    const char *child;
    size_t child_length;
    while (mp_json_next(parts, parts_length, &offset, &child, &child_length)) {
        if (gmail_find_part(child, child_length, mime, found, depth + 1)) {
            return true;
        }
    }
    return false;
}

static esp_err_t gmail_attachment(const char *message_id, const char *attachment_id,
                                  char *output, size_t size)
{
    mp_url_encode(attachment_id, s_encoded, sizeof(s_encoded));
    snprintf(s_url, sizeof(s_url),
             "https://gmail.googleapis.com/gmail/v1/users/me/messages/%s/attachments/%s",
             message_id, s_encoded);
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_GET, s_url, NULL, 0, NULL, NULL, 20000,
                                         "application/json", &response, output, size);
    if (result != ESP_OK) {
        return result;
    }
    const char *data;
    size_t data_length;
    const char *json = mp_google_response();
    if (!mp_json_get_slice(json, strlen(json), "data", &data, &data_length)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return base64_decode(data, data_length, s_decoded, sizeof(s_decoded));
}

static void gmail_labels(const char *response, mp_writer_t *writer)
{
    const char *labels;
    size_t labels_length;
    if (!mp_json_get_slice(response, strlen(response), "labelIds", &labels, &labels_length)) {
        return;
    }
    mp_writer_raw(writer, "Labels: ");
    size_t offset = 0;
    const char *label;
    size_t label_length;
    char decoded[64];
    bool first = true;
    while (mp_json_next(labels, labels_length, &offset, &label, &label_length)) {
        if (mp_json_decode_string(label, label_length, decoded, sizeof(decoded))) {
            if (!first) {
                mp_writer_raw(writer, ", ");
            }
            mp_writer_raw(writer, decoded);
            first = false;
        }
    }
    mp_writer_char(writer, '\n');
}

static esp_err_t gmail_send(const mp_email_t *email, char *output, size_t size)
{
    if (!email || !email->to || !email->subject || !email->body ||
        strchr(email->to, '\r') || strchr(email->to, '\n') ||
        strchr(email->subject, '\r') || strchr(email->subject, '\n')) {
        return ESP_ERR_INVALID_ARG;
    }
    int length = snprintf(s_email_header, sizeof(s_email_header),
                          "To: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n",
                          email->to, email->subject);
    if (length < 0 || (size_t)length >= sizeof(s_email_header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t raw_size = (size_t)length + strlen(email->body);
    if (raw_size < (size_t)length || raw_size > (SIZE_MAX - 10) / 4 * 3) {
        return ESP_ERR_INVALID_SIZE;
    }
    email_write_context_t context = {
        .header = s_email_header,
        .header_size = (size_t)length,
        .body = email->body
    };
    mp_http_response_t response;
    esp_err_t result = mp_google_request(
        HTTP_METHOD_POST, "https://gmail.googleapis.com/gmail/v1/users/me/messages/send",
        NULL, base64url_size(raw_size) + 10, email_write, &context, 60000,
        "application/json", &response, output, size);
    if (result == ESP_OK) {
        strlcpy(output, "Email sent.", size);
    }
    return result;
}

static esp_err_t gmail_search(const char *query, const char *page_token, uint32_t page_size,
                              char *output, size_t size)
{
    if (!query || !page_token || page_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (page_size > GMAIL_PAGE_SIZE) {
        page_size = GMAIL_PAGE_SIZE;
    }
    mp_writer_t url;
    mp_writer_init(&url, s_url, sizeof(s_url));
    mp_writer_format(&url,
                     "https://gmail.googleapis.com/gmail/v1/users/me/messages?maxResults=%lu",
                     (unsigned long)page_size);
    if (query[0]) {
        mp_url_encode(query, s_encoded, sizeof(s_encoded));
        mp_writer_format(&url, "&q=%s", s_encoded);
    }
    if (page_token[0]) {
        mp_url_encode(page_token, s_encoded, sizeof(s_encoded));
        mp_writer_format(&url, "&pageToken=%s", s_encoded);
    }
    if (!url.valid) {
        return ESP_ERR_INVALID_SIZE;
    }
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_GET, s_url, NULL, 0, NULL, NULL, 20000,
                                         "application/json", &response, output, size);
    if (result != ESP_OK) {
        return result;
    }
    size_t count = gmail_ids(mp_google_response());
    if (!count) {
        strlcpy(output, "No email messages matched.", size);
        return ESP_OK;
    }
    mp_writer_t writer;
    mp_writer_init(&writer, output, size);
    for (size_t index = 0; index < count; index++) {
        result = gmail_metadata(s_ids[index].id, (uint32_t)index + 1, &writer, output, size);
        if (result != ESP_OK) {
            return result;
        }
    }
    if (s_next_page[0]) {
        mp_writer_format(&writer, "Next page token: %s", s_next_page);
    } else {
        mp_writer_raw(&writer, "No more pages.");
    }
    return writer.valid ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t gmail_get(const char *id, char *output, size_t size)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_url, sizeof(s_url),
             "https://gmail.googleapis.com/gmail/v1/users/me/messages/%s?format=full", id);
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_GET, s_url, NULL, 0, NULL, NULL, 30000,
                                         "application/json", &response, output, size);
    if (result != ESP_OK) {
        return result;
    }
    const char *json = mp_google_response();
    size_t length = strlen(json);
    const char *payload;
    size_t payload_length;
    char from[256] = "";
    char to[256] = "";
    char subject[512] = "";
    char date[128] = "";
    char snippet[512] = "";
    char labels[256] = "";
    if (!mp_json_get_slice(json, length, "payload", &payload, &payload_length)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    gmail_header(payload, payload_length, "From", from, sizeof(from));
    gmail_header(payload, payload_length, "To", to, sizeof(to));
    gmail_header(payload, payload_length, "Subject", subject, sizeof(subject));
    gmail_header(payload, payload_length, "Date", date, sizeof(date));
    mp_json_get_string(json, length, "snippet", snippet, sizeof(snippet));
    mp_writer_t label_writer;
    mp_writer_init(&label_writer, labels, sizeof(labels));
    gmail_labels(json, &label_writer);
    gmail_part_t part = {0};
    if (!gmail_find_part(payload, payload_length, "text/plain", &part, 0)) {
        gmail_find_part(payload, payload_length, "text/html", &part, 0);
    }
    if (part.data) {
        result = base64_decode(part.data, part.data_length, s_decoded, sizeof(s_decoded));
    } else if (part.attachment_id[0]) {
        result = gmail_attachment(id, part.attachment_id, output, size);
    } else {
        s_decoded[0] = 0;
    }
    if (result != ESP_OK) {
        return result;
    }
    mp_writer_t writer;
    mp_writer_init(&writer, output, size);
    mp_writer_format(&writer, "%s\nID: %s\nFrom: %s\nTo: %s\nDate: %s\n",
                     subject[0] ? subject : "(no subject)", id,
                     from[0] ? from : "(unknown)", to[0] ? to : "(unknown)",
                     date[0] ? date : "(unknown)");
    mp_writer_raw(&writer, labels);
    mp_writer_raw(&writer, "\n");
    if (part.html && s_decoded[0] && writer.valid) {
        mp_html_parser_t parser;
        mp_html_parser_init(&parser, output + writer.length, size - writer.length);
        for (size_t index = 0; s_decoded[index]; index++) {
            if (!mp_html_parser_push(&parser, s_decoded[index])) {
                break;
            }
        }
        mp_html_parser_finish(&parser);
    } else {
        mp_writer_raw(&writer, s_decoded[0] ? s_decoded : snippet);
    }
    return writer.valid ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t gmail_modify(const char *id, const char *action, char *output, size_t size)
{
    if (!id || !id[0] || !action) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *body = NULL;
    if (strcmp(action, "mark_read") == 0) {
        body = "{\"removeLabelIds\":[\"UNREAD\"]}";
    } else if (strcmp(action, "mark_unread") == 0) {
        body = "{\"addLabelIds\":[\"UNREAD\"]}";
    } else if (strcmp(action, "archive") == 0) {
        body = "{\"removeLabelIds\":[\"INBOX\"]}";
    } else if (strcmp(action, "move_to_inbox") == 0) {
        body = "{\"addLabelIds\":[\"INBOX\"]}";
    } else if (strcmp(action, "star") == 0) {
        body = "{\"addLabelIds\":[\"STARRED\"]}";
    } else if (strcmp(action, "unstar") == 0) {
        body = "{\"removeLabelIds\":[\"STARRED\"]}";
    } else if (strcmp(action, "mark_spam") == 0) {
        body = "{\"addLabelIds\":[\"SPAM\"],\"removeLabelIds\":[\"INBOX\"]}";
    } else if (strcmp(action, "not_spam") == 0) {
        body = "{\"removeLabelIds\":[\"SPAM\"]}";
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_url, sizeof(s_url),
             "https://gmail.googleapis.com/gmail/v1/users/me/messages/%s/modify", id);
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_POST, s_url, body, strlen(body), NULL, NULL,
                                         20000, "application/json", &response, output, size);
    if (result == ESP_OK) {
        snprintf(output, size, "Email action completed: %s.", action);
    }
    return result;
}

static esp_err_t gmail_trash(const char *id, char *output, size_t size)
{
    return gmail_move(id, "trash", output, size);
}

static esp_err_t gmail_untrash(const char *id, char *output, size_t size)
{
    return gmail_move(id, "untrash", output, size);
}

static esp_err_t gmail_move(const char *id, const char *operation, char *output, size_t size)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_url, sizeof(s_url),
             "https://gmail.googleapis.com/gmail/v1/users/me/messages/%s/%s", id, operation);
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_POST, s_url, NULL, 0, NULL, NULL, 20000,
                                         "application/json", &response, output, size);
    if (result == ESP_OK) {
        snprintf(output, size, "Email %s completed.", operation);
    }
    return result;
}

static esp_err_t write_all(esp_http_client_handle_t client, const char *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        int count = esp_http_client_write(client, data + written, size - written);
        if (count <= 0) {
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        written += count;
    }
    return ESP_OK;
}

static esp_err_t base64_flush(base64_stream_t *stream)
{
    esp_err_t result = write_all(stream->client, s_encode_buffer, stream->output_size);
    stream->output_size = 0;
    return result;
}

static esp_err_t base64_emit(base64_stream_t *stream, const uint8_t *input, size_t size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (stream->output_size + 4 > sizeof(s_encode_buffer)) {
        esp_err_t result = base64_flush(stream);
        if (result != ESP_OK) {
            return result;
        }
    }
    uint32_t value = (uint32_t)input[0] << 16;
    if (size > 1) {
        value |= (uint32_t)input[1] << 8;
    }
    if (size > 2) {
        value |= input[2];
    }
    s_encode_buffer[stream->output_size++] = alphabet[(value >> 18) & 63];
    s_encode_buffer[stream->output_size++] = alphabet[(value >> 12) & 63];
    if (size > 1) {
        s_encode_buffer[stream->output_size++] = alphabet[(value >> 6) & 63];
    }
    if (size > 2) {
        s_encode_buffer[stream->output_size++] = alphabet[value & 63];
    }
    return ESP_OK;
}

static esp_err_t base64_feed(base64_stream_t *stream, const uint8_t *input, size_t size)
{
    if (stream->tail_size) {
        while (stream->tail_size < 3 && size) {
            stream->tail[stream->tail_size++] = *input++;
            size--;
        }
        if (stream->tail_size == 3) {
            esp_err_t result = base64_emit(stream, stream->tail, 3);
            if (result != ESP_OK) {
                return result;
            }
            stream->tail_size = 0;
        }
    }
    while (size >= 3) {
        esp_err_t result = base64_emit(stream, input, 3);
        if (result != ESP_OK) {
            return result;
        }
        input += 3;
        size -= 3;
    }
    while (size) {
        stream->tail[stream->tail_size++] = *input++;
        size--;
    }
    return ESP_OK;
}

static esp_err_t base64_finish(base64_stream_t *stream)
{
    if (stream->tail_size) {
        esp_err_t result = base64_emit(stream, stream->tail, stream->tail_size);
        if (result != ESP_OK) {
            return result;
        }
    }
    return base64_flush(stream);
}

static esp_err_t email_write(esp_http_client_handle_t client, void *context)
{
    email_write_context_t *email = context;
    esp_err_t result = write_all(client, "{\"raw\":\"", 8);
    base64_stream_t stream = {.client = client};
    if (result == ESP_OK) {
        result = base64_feed(&stream, (const uint8_t *)email->header, email->header_size);
    }
    if (result == ESP_OK) {
        result = base64_feed(&stream, (const uint8_t *)email->body, strlen(email->body));
    }
    if (result == ESP_OK) {
        result = base64_finish(&stream);
    }
    if (result == ESP_OK) {
        result = write_all(client, "\"}", 2);
    }
    return result;
}

static size_t base64url_size(size_t size)
{
    size_t encoded = size / 3 * 4;
    size_t remainder = size % 3;
    return encoded + (remainder ? remainder + 1 : 0);
}

const mp_email_service_t *mp_email_service(void)
{
    return &s_service;
}
