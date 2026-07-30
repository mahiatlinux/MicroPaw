#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mp_time.h"
#include "unity.h"

static void assert_local(const char *date, const char *clock, const char *meridiem,
                         const char *expected);

static void assert_local(const char *date, const char *clock, const char *meridiem,
                         const char *expected)
{
    int64_t epoch;
    char actual[24];
    TEST_ASSERT_EQUAL(ESP_OK, mp_time_parse_local(date, clock, meridiem, &epoch));
    time_t parsed = (time_t)epoch;
    struct tm local;
    localtime_r(&parsed, &local);
    strftime(actual, sizeof(actual), "%Y-%m-%d %H:%M", &local);
    TEST_ASSERT_EQUAL_STRING(expected, actual);
}

TEST_CASE("local scheduler time preserves AM and PM", "[micropaw][time]")
{
    setenv("TZ", "UTC0", 1);
    tzset();
    assert_local("2030-01-02", "12:00", "AM", "2030-01-02 00:00");
    assert_local("2030-01-02", "12:00", "PM", "2030-01-02 12:00");
    assert_local("2030-01-02", "01:45", "AM", "2030-01-02 01:45");
    assert_local("2030-01-02", "01:45", "PM", "2030-01-02 13:45");
    assert_local("2030-01-02", "23:10", "24H", "2030-01-02 23:10");
}

TEST_CASE("local scheduler time rejects invalid and missing wall times", "[micropaw][time]")
{
    int64_t epoch;
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mp_time_parse_local("2030-02-30", "01:00", "AM", &epoch));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mp_time_parse_local("2030-01-02", "00:30", "AM", &epoch));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mp_time_parse_local("2030-01-02", "13:30", "PM", &epoch));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mp_time_parse_local("2030-01-02", "24:00", "24H", &epoch));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      mp_time_parse_local("2030-03-10", "02:30", "AM", &epoch));
}

TEST_CASE("local scheduler time applies configured daylight saving offset", "[micropaw][time]")
{
    int64_t epoch;
    char offset[8];
    setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1);
    tzset();
    TEST_ASSERT_EQUAL(ESP_OK,
                      mp_time_parse_local("2030-01-02", "08:00", "AM", &epoch));
    time_t summer = (time_t)epoch;
    struct tm local;
    localtime_r(&summer, &local);
    strftime(offset, sizeof(offset), "%z", &local);
    TEST_ASSERT_EQUAL_STRING("+1300", offset);
    TEST_ASSERT_EQUAL(ESP_OK,
                      mp_time_parse_local("2030-06-02", "08:00", "AM", &epoch));
    time_t winter = (time_t)epoch;
    localtime_r(&winter, &local);
    strftime(offset, sizeof(offset), "%z", &local);
    TEST_ASSERT_EQUAL_STRING("+1200", offset);
}

TEST_CASE("daily scheduler repeats keep their local clock through daylight saving", "[micropaw][time]")
{
    int64_t epoch;
    char local_text[24];
    setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1);
    tzset();
    TEST_ASSERT_EQUAL(ESP_OK,
                      mp_time_parse_local("2030-09-28", "08:00", "AM", &epoch));
    int64_t next = mp_time_repeat_next(epoch, 86400);
    time_t parsed = (time_t)next;
    struct tm local;
    localtime_r(&parsed, &local);
    strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M %z", &local);
    TEST_ASSERT_EQUAL_STRING("2030-09-29 08:00 +1300", local_text);
    TEST_ASSERT_EQUAL_INT32(23 * 60 * 60, (int32_t)(next - epoch));
}
