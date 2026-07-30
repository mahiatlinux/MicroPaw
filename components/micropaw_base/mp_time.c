#include "mp_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static bool parse_date(const char *text, int *year, int *month, int *day);
static bool parse_clock(const char *text, int *hour, int *minute);
esp_err_t mp_time_parse_local(const char *date, const char *clock,
                              const char *meridiem, int64_t *epoch);
int64_t mp_time_repeat_next(int64_t epoch, uint32_t repeat_seconds);

static bool parse_date(const char *text, int *year, int *month, int *day)
{
    char extra;
    return text && strlen(text) == 10 &&
           sscanf(text, "%4d-%2d-%2d%c", year, month, day, &extra) == 3 &&
           *year >= 2023 && *year <= 2100 && *month >= 1 && *month <= 12 &&
           *day >= 1 && *day <= 31;
}

static bool parse_clock(const char *text, int *hour, int *minute)
{
    char extra;
    return text && strlen(text) == 5 &&
           sscanf(text, "%2d:%2d%c", hour, minute, &extra) == 2 &&
           *minute >= 0 && *minute < 60;
}

esp_err_t mp_time_parse_local(const char *date, const char *clock,
                              const char *meridiem, int64_t *epoch)
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    if (!epoch || !meridiem || !parse_date(date, &year, &month, &day) ||
        !parse_clock(clock, &hour, &minute)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(meridiem, "AM") == 0 || strcmp(meridiem, "PM") == 0) {
        if (hour < 1 || hour > 12) {
            return ESP_ERR_INVALID_ARG;
        }
        hour = hour % 12 + (strcmp(meridiem, "PM") == 0 ? 12 : 0);
    } else if (strcmp(meridiem, "24H") != 0 || hour < 0 || hour > 23) {
        return ESP_ERR_INVALID_ARG;
    }
    struct tm local = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = minute,
        .tm_isdst = -1
    };
    time_t parsed = mktime(&local);
    struct tm checked;
    if (parsed < 0 || !localtime_r(&parsed, &checked) ||
        checked.tm_year != year - 1900 || checked.tm_mon != month - 1 ||
        checked.tm_mday != day || checked.tm_hour != hour ||
        checked.tm_min != minute) {
        return ESP_ERR_INVALID_ARG;
    }
    *epoch = (int64_t)parsed;
    return ESP_OK;
}

int64_t mp_time_repeat_next(int64_t epoch, uint32_t repeat_seconds)
{
    if (!repeat_seconds || repeat_seconds % 86400) {
        return epoch + repeat_seconds;
    }
    time_t current = (time_t)epoch;
    struct tm local;
    if (!localtime_r(&current, &local)) {
        return epoch + repeat_seconds;
    }
    local.tm_mday += repeat_seconds / 86400;
    local.tm_isdst = -1;
    time_t next = mktime(&local);
    return next < 0 ? epoch + repeat_seconds : (int64_t)next;
}
