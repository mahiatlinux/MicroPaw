#include <string.h>

#include "mp_agent_policy.h"
#include "unity.h"

static uint8_t missing_for(const char *name, uint8_t reads);
static void assert_error(const char *name, uint8_t reads, const char *expected);

static uint8_t missing_for(const char *name, uint8_t reads)
{
    return mp_agent_policy_required(name) & ~reads;
}

static void assert_error(const char *name, uint8_t reads, const char *expected)
{
    char output[160];
    mp_agent_policy_error(name, missing_for(name, reads), output, sizeof(output));
    TEST_ASSERT_EQUAL_STRING(expected, output);
}

TEST_CASE("new schedules require an earlier clock result", "[micropaw][policy]")
{
    TEST_ASSERT_EQUAL(MP_AGENT_READ_TIME, missing_for("email_schedule", 0));
    assert_error("email_schedule", 0,
                 "Call time_now, use its result, then retry email_schedule.");
    uint8_t reads_before_batch = 0;
    uint8_t reads_after_batch = reads_before_batch | mp_agent_policy_read("time_now");
    TEST_ASSERT_EQUAL(MP_AGENT_READ_TIME,
                      missing_for("email_schedule", reads_before_batch));
    TEST_ASSERT_EQUAL(0, missing_for("email_schedule", reads_after_batch));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_TIME, mp_agent_policy_required("schedule_add"));
    TEST_ASSERT_EQUAL(0, missing_for("schedule_add", MP_AGENT_READ_TIME));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_TIME,
                      missing_for("schedule_add", MP_AGENT_READ_SCHEDULE));
}

TEST_CASE("schedule changes use exact prerequisite masks", "[micropaw][policy]")
{
    TEST_ASSERT_EQUAL(MP_AGENT_READ_SCHEDULE | MP_AGENT_READ_TIME,
                      mp_agent_policy_required("schedule_update"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_SCHEDULE | MP_AGENT_READ_TIME,
                      mp_agent_policy_required("email_schedule_update"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_SCHEDULE | MP_AGENT_READ_TIME,
                      mp_agent_policy_required("schedule_snooze"));
    assert_error("schedule_update", 0,
                 "Call schedule_list and time_now, use their results, then retry schedule_update.");
    assert_error("schedule_update", MP_AGENT_READ_SCHEDULE,
                 "Call time_now, use its result, then retry schedule_update.");
    assert_error("schedule_update", MP_AGENT_READ_TIME,
                 "Call schedule_list, use its result, then retry schedule_update.");
    for (const char **name = (const char *[]){"schedule_run", "schedule_delete", NULL};
         *name; name++) {
        TEST_ASSERT_EQUAL(MP_AGENT_READ_SCHEDULE, mp_agent_policy_required(*name));
        TEST_ASSERT_EQUAL(0, missing_for(*name, MP_AGENT_READ_SCHEDULE));
    }
}

TEST_CASE("stored state writes use matching reads", "[micropaw][policy]")
{
    TEST_ASSERT_EQUAL(MP_AGENT_READ_MEMORY, mp_agent_policy_required("memory_save"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_MISSED,
                      mp_agent_policy_required("schedule_missed_clear"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_EMAIL, mp_agent_policy_required("email_modify"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_EMAIL, mp_agent_policy_required("email_trash"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_EMAIL, mp_agent_policy_required("email_untrash"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_CALENDAR,
                      mp_agent_policy_required("calendar_delete"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_CALENDAR | MP_AGENT_READ_TIME,
                      mp_agent_policy_required("calendar_create"));
    TEST_ASSERT_EQUAL(MP_AGENT_READ_CALENDAR | MP_AGENT_READ_TIME,
                      mp_agent_policy_required("calendar_update"));
}

TEST_CASE("fallbacks belong only to requested actions", "[micropaw][policy]")
{
    TEST_ASSERT_EQUAL_STRING("I'll check the time and schedule that email.",
                             mp_agent_policy_fallback("email_schedule"));
    TEST_ASSERT_EQUAL_STRING("I'll check the time and set that reminder.",
                             mp_agent_policy_fallback("schedule_add"));
    for (const char **name = (const char *[]){"schedule_update", "email_schedule_update",
                                             "schedule_snooze", "schedule_run",
                                             "schedule_delete", NULL};
         *name; name++) {
        TEST_ASSERT_EQUAL_STRING("I'll check your reminders and update that one.",
                                 mp_agent_policy_fallback(*name));
    }
    for (const char **name = (const char *[]){"email_send", "email_modify", "email_trash",
                                             "email_untrash", NULL};
         *name; name++) {
        TEST_ASSERT_EQUAL_STRING("I'll handle that email.",
                                 mp_agent_policy_fallback(*name));
    }
    for (const char **name = (const char *[]){"calendar_create", "calendar_update",
                                             "calendar_delete", NULL};
         *name; name++) {
        TEST_ASSERT_EQUAL_STRING("I'll check your calendar and update it.",
                                 mp_agent_policy_fallback(*name));
    }
    for (const char **name = (const char *[]){"time_now", "schedule_list", "memory_list",
                                             "email_search", "calendar_list", "web_search",
                                             "diagnostics", NULL};
         *name; name++) {
        TEST_ASSERT_NULL(mp_agent_policy_fallback(*name));
    }
}

TEST_CASE("slow progress advances only for a new network stage", "[micropaw][policy]")
{
    TEST_ASSERT_EQUAL(MP_AGENT_SLOW_WEB, mp_agent_policy_slow_stage("web_fetch"));
    TEST_ASSERT_EQUAL(MP_AGENT_SLOW_EMAIL, mp_agent_policy_slow_stage("email_search"));
    TEST_ASSERT_EQUAL(MP_AGENT_SLOW_CALENDAR,
                      mp_agent_policy_slow_stage("calendar_list"));
    TEST_ASSERT_EQUAL(0, mp_agent_policy_slow_stage("email_schedule"));
    TEST_ASSERT_EQUAL(0, mp_agent_policy_slow_stage("email_schedule_update"));
    uint8_t reported = MP_AGENT_SLOW_EMAIL | MP_AGENT_SLOW_CALENDAR;
    TEST_ASSERT_EQUAL(0, mp_agent_policy_slow_stage("calendar_get") & ~reported);
    TEST_ASSERT_EQUAL(MP_AGENT_SLOW_WEB,
                      mp_agent_policy_slow_stage("web_search") & ~reported);
}
