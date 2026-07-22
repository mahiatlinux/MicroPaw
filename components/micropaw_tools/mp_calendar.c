#include "mp_services.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "mp_google.h"
#include "mp_json.h"

#define CALENDAR_PAGE_SIZE 20

EXT_RAM_BSS_ATTR static char s_url[4096];
EXT_RAM_BSS_ATTR static char s_encoded[2048];
EXT_RAM_BSS_ATTR static char s_body[4096];

static bool calendar_time(const char *event, size_t length, const char *key,
                          char *output, size_t size);
static void calendar_event(const char *event, size_t length, uint32_t number,
                           mp_writer_t *writer);
static esp_err_t calendar_create(const mp_calendar_event_t *event, char *output, size_t size);
static esp_err_t calendar_list(const char *query, const char *time_min, const char *time_max,
                               const char *page_token, uint32_t page_size,
                               char *output, size_t size);
static esp_err_t calendar_get(const char *id, char *output, size_t size);
static esp_err_t calendar_update(const char *id, const mp_calendar_event_t *event,
                                 char *output, size_t size);
static esp_err_t calendar_remove(const char *id, const char *send_updates,
                                 char *output, size_t size);
static esp_err_t calendar_write(esp_http_client_method_t method, const char *url,
                                const mp_calendar_event_t *event, char *output, size_t size);
const mp_calendar_service_t *mp_calendar_service(void);

static const mp_calendar_service_t s_service = {
    "google_calendar", calendar_create, calendar_list, calendar_get,
    calendar_update, calendar_remove
};

static bool calendar_time(const char *event, size_t length, const char *key,
                          char *output, size_t size)
{
    const char *value;
    size_t value_length;
    if (!mp_json_get_slice(event, length, key, &value, &value_length)) {
        output[0] = 0;
        return false;
    }
    if (mp_json_get_string(value, value_length, "dateTime", output, size)) {
        return true;
    }
    return mp_json_get_string(value, value_length, "date", output, size);
}

static void calendar_event(const char *event, size_t length, uint32_t number,
                           mp_writer_t *writer)
{
    char id[256] = "";
    char summary[512] = "";
    char start[128] = "";
    char end[128] = "";
    char status[32] = "";
    mp_json_get_string(event, length, "id", id, sizeof(id));
    mp_json_get_string(event, length, "summary", summary, sizeof(summary));
    mp_json_get_string(event, length, "status", status, sizeof(status));
    calendar_time(event, length, "start", start, sizeof(start));
    calendar_time(event, length, "end", end, sizeof(end));
    mp_writer_format(writer, "%lu. %s\nID: %s\nStart: %s\nEnd: %s\nStatus: %s\n\n",
                     (unsigned long)number, summary[0] ? summary : "(no title)", id,
                     start, end, status);
}

static esp_err_t calendar_create(const mp_calendar_event_t *event, char *output, size_t size)
{
    return calendar_write(HTTP_METHOD_POST,
                          "https://www.googleapis.com/calendar/v3/calendars/primary/events",
                          event, output, size);
}

static esp_err_t calendar_list(const char *query, const char *time_min, const char *time_max,
                               const char *page_token, uint32_t page_size,
                               char *output, size_t size)
{
    if (!query || !time_min || !time_max || !page_token || page_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (page_size > CALENDAR_PAGE_SIZE) {
        page_size = CALENDAR_PAGE_SIZE;
    }
    mp_writer_t url;
    mp_writer_init(&url, s_url, sizeof(s_url));
    mp_writer_format(&url,
                     "https://www.googleapis.com/calendar/v3/calendars/primary/events?singleEvents=true&orderBy=startTime&showDeleted=false&maxResults=%lu",
                     (unsigned long)page_size);
    if (query[0]) {
        mp_url_encode(query, s_encoded, sizeof(s_encoded));
        mp_writer_format(&url, "&q=%s", s_encoded);
    }
    if (time_min[0]) {
        mp_url_encode(time_min, s_encoded, sizeof(s_encoded));
        mp_writer_format(&url, "&timeMin=%s", s_encoded);
    }
    if (time_max[0]) {
        mp_url_encode(time_max, s_encoded, sizeof(s_encoded));
        mp_writer_format(&url, "&timeMax=%s", s_encoded);
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
    const char *json = mp_google_response();
    size_t length = strlen(json);
    const char *items;
    size_t items_length;
    if (!mp_json_get_slice(json, length, "items", &items, &items_length)) {
        strlcpy(output, "No calendar events matched.", size);
        return ESP_OK;
    }
    char next_page[1024] = "";
    mp_json_get_string(json, length, "nextPageToken", next_page, sizeof(next_page));
    mp_writer_t writer;
    mp_writer_init(&writer, output, size);
    size_t offset = 0;
    const char *event;
    size_t event_length;
    uint32_t number = 1;
    while (mp_json_next(items, items_length, &offset, &event, &event_length)) {
        calendar_event(event, event_length, number++, &writer);
    }
    if (number == 1) {
        mp_writer_raw(&writer, "No calendar events matched.");
    } else if (next_page[0]) {
        mp_writer_format(&writer, "Next page token: %s", next_page);
    } else {
        mp_writer_raw(&writer, "No more pages.");
    }
    return writer.valid ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t calendar_get(const char *id, char *output, size_t size)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_url_encode(id, s_encoded, sizeof(s_encoded));
    snprintf(s_url, sizeof(s_url),
             "https://www.googleapis.com/calendar/v3/calendars/primary/events/%s", s_encoded);
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_GET, s_url, NULL, 0, NULL, NULL, 20000,
                                         "application/json", &response, output, size);
    if (result != ESP_OK) {
        return result;
    }
    const char *json = mp_google_response();
    size_t length = strlen(json);
    char summary[512] = "";
    char start[128] = "";
    char end[128] = "";
    char status[32] = "";
    char location[512] = "";
    char description[2048] = "";
    char link[1024] = "";
    mp_json_get_string(json, length, "summary", summary, sizeof(summary));
    mp_json_get_string(json, length, "status", status, sizeof(status));
    mp_json_get_string(json, length, "location", location, sizeof(location));
    mp_json_get_string(json, length, "description", description, sizeof(description));
    mp_json_get_string(json, length, "htmlLink", link, sizeof(link));
    calendar_time(json, length, "start", start, sizeof(start));
    calendar_time(json, length, "end", end, sizeof(end));
    mp_writer_t writer;
    mp_writer_init(&writer, output, size);
    mp_writer_format(&writer,
                     "%s\nID: %s\nStart: %s\nEnd: %s\nStatus: %s\nLocation: %s\nDescription: %s\nLink: %s",
                     summary[0] ? summary : "(no title)", id, start, end, status,
                     location, description, link);
    return writer.valid ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t calendar_update(const char *id, const mp_calendar_event_t *event,
                                 char *output, size_t size)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_url_encode(id, s_encoded, sizeof(s_encoded));
    snprintf(s_url, sizeof(s_url),
             "https://www.googleapis.com/calendar/v3/calendars/primary/events/%s", s_encoded);
    return calendar_write(HTTP_METHOD_PATCH, s_url, event, output, size);
}

static esp_err_t calendar_remove(const char *id, const char *send_updates,
                                 char *output, size_t size)
{
    if (!id || !id[0] || !send_updates ||
        (strcmp(send_updates, "none") != 0 && strcmp(send_updates, "all") != 0 &&
         strcmp(send_updates, "externalOnly") != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded_id[768];
    char encoded_updates[64];
    mp_url_encode(id, encoded_id, sizeof(encoded_id));
    mp_url_encode(send_updates, encoded_updates, sizeof(encoded_updates));
    snprintf(s_url, sizeof(s_url),
             "https://www.googleapis.com/calendar/v3/calendars/primary/events/%s?sendUpdates=%s",
             encoded_id, encoded_updates);
    mp_http_response_t response;
    esp_err_t result = mp_google_request(HTTP_METHOD_DELETE, s_url, NULL, 0, NULL, NULL, 20000,
                                         NULL, &response, output, size);
    if (result == ESP_OK) {
        strlcpy(output, "Calendar event deleted.", size);
    }
    return result;
}

static esp_err_t calendar_write(esp_http_client_method_t method, const char *url,
                                const mp_calendar_event_t *event, char *output, size_t size)
{
    if (!event || !event->summary || !event->start_rfc3339 || !event->end_rfc3339 ||
        (!event->summary[0] && !event->start_rfc3339[0] && !event->end_rfc3339[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    mp_writer_t writer;
    mp_writer_init(&writer, s_body, sizeof(s_body));
    mp_writer_char(&writer, '{');
    bool comma = false;
    if (event->summary[0]) {
        mp_writer_raw(&writer, "\"summary\":");
        mp_writer_string(&writer, event->summary);
        comma = true;
    }
    if (event->start_rfc3339[0]) {
        if (comma) {
            mp_writer_char(&writer, ',');
        }
        mp_writer_raw(&writer, "\"start\":{\"dateTime\":");
        mp_writer_string(&writer, event->start_rfc3339);
        mp_writer_char(&writer, '}');
        comma = true;
    }
    if (event->end_rfc3339[0]) {
        if (comma) {
            mp_writer_char(&writer, ',');
        }
        mp_writer_raw(&writer, "\"end\":{\"dateTime\":");
        mp_writer_string(&writer, event->end_rfc3339);
        mp_writer_char(&writer, '}');
    }
    mp_writer_char(&writer, '}');
    if (!writer.valid) {
        return ESP_ERR_INVALID_SIZE;
    }
    mp_http_response_t response;
    esp_err_t result = mp_google_request(method, url, s_body, writer.length, NULL, NULL, 20000,
                                         "application/json", &response, output, size);
    if (result == ESP_OK) {
        strlcpy(output, method == HTTP_METHOD_POST ? "Calendar event created." :
                "Calendar event updated.", size);
    }
    return result;
}

const mp_calendar_service_t *mp_calendar_service(void)
{
    return &s_service;
}
