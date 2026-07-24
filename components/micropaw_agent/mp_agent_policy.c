#include "mp_agent_policy.h"

#include <stdio.h>
#include <string.h>

uint8_t mp_agent_policy_read(const char *name);
uint8_t mp_agent_policy_required(const char *name);
uint8_t mp_agent_policy_slow_stage(const char *name);
const char *mp_agent_policy_fallback(const char *name);
void mp_agent_policy_error(const char *name, uint8_t missing, char *output, size_t size);

uint8_t mp_agent_policy_read(const char *name)
{
    if (strcmp(name, "memory_list") == 0) {
        return MP_AGENT_READ_MEMORY;
    }
    if (strcmp(name, "schedule_list") == 0) {
        return MP_AGENT_READ_SCHEDULE;
    }
    if (strcmp(name, "schedule_missed_list") == 0) {
        return MP_AGENT_READ_MISSED;
    }
    if (strcmp(name, "time_now") == 0) {
        return MP_AGENT_READ_TIME;
    }
    if (strcmp(name, "email_search") == 0 || strcmp(name, "email_get") == 0) {
        return MP_AGENT_READ_EMAIL;
    }
    if (strcmp(name, "calendar_list") == 0 || strcmp(name, "calendar_get") == 0) {
        return MP_AGENT_READ_CALENDAR;
    }
    return 0;
}

uint8_t mp_agent_policy_required(const char *name)
{
    if (strcmp(name, "memory_save") == 0) {
        return MP_AGENT_READ_MEMORY;
    }
    if (strcmp(name, "schedule_add") == 0 || strcmp(name, "email_schedule") == 0) {
        return MP_AGENT_READ_TIME;
    }
    if (strcmp(name, "schedule_update") == 0) {
        return MP_AGENT_READ_SCHEDULE | MP_AGENT_READ_TIME;
    }
    if (strcmp(name, "schedule_snooze") == 0 ||
        strcmp(name, "schedule_run") == 0 ||
        strcmp(name, "schedule_delete") == 0) {
        return MP_AGENT_READ_SCHEDULE;
    }
    if (strcmp(name, "schedule_missed_clear") == 0) {
        return MP_AGENT_READ_MISSED;
    }
    if (strcmp(name, "email_modify") == 0 ||
        strcmp(name, "email_trash") == 0 ||
        strcmp(name, "email_untrash") == 0) {
        return MP_AGENT_READ_EMAIL;
    }
    if (strcmp(name, "calendar_create") == 0 ||
        strcmp(name, "calendar_update") == 0) {
        return MP_AGENT_READ_CALENDAR | MP_AGENT_READ_TIME;
    }
    if (strcmp(name, "calendar_delete") == 0) {
        return MP_AGENT_READ_CALENDAR;
    }
    return 0;
}

uint8_t mp_agent_policy_slow_stage(const char *name)
{
    if (strcmp(name, "web_search") == 0 ||
        strcmp(name, "web_fetch") == 0 ||
        strcmp(name, "rss_read") == 0) {
        return MP_AGENT_SLOW_WEB;
    }
    if (strncmp(name, "email_", 6) == 0 && strcmp(name, "email_schedule") != 0) {
        return MP_AGENT_SLOW_EMAIL;
    }
    return strncmp(name, "calendar_", 9) == 0 ? MP_AGENT_SLOW_CALENDAR : 0;
}

const char *mp_agent_policy_fallback(const char *name)
{
    if (strcmp(name, "email_schedule") == 0) {
        return "I'll check the time and schedule that email.";
    }
    if (strcmp(name, "schedule_add") == 0) {
        return "I'll check the time and set that reminder.";
    }
    if (strcmp(name, "schedule_update") == 0 ||
        strcmp(name, "schedule_snooze") == 0 ||
        strcmp(name, "schedule_run") == 0 ||
        strcmp(name, "schedule_delete") == 0) {
        return "I'll check your reminders and update that one.";
    }
    if (strcmp(name, "email_send") == 0 ||
        strcmp(name, "email_modify") == 0 ||
        strcmp(name, "email_trash") == 0 ||
        strcmp(name, "email_untrash") == 0) {
        return "I'll handle that email.";
    }
    if (strcmp(name, "calendar_create") == 0 ||
        strcmp(name, "calendar_update") == 0 ||
        strcmp(name, "calendar_delete") == 0) {
        return "I'll check your calendar and update it.";
    }
    return NULL;
}

void mp_agent_policy_error(const char *name, uint8_t missing, char *output, size_t size)
{
    if (missing == MP_AGENT_READ_TIME) {
        snprintf(output, size, "Call time_now, use its result, then retry %s.", name);
    } else if (missing == MP_AGENT_READ_SCHEDULE) {
        snprintf(output, size, "Call schedule_list, use its result, then retry %s.", name);
    } else if (missing == MP_AGENT_READ_MEMORY) {
        snprintf(output, size, "Call memory_list, use its result, then retry %s.", name);
    } else if (missing == MP_AGENT_READ_EMAIL) {
        snprintf(output, size, "Call email_search or email_get, use its result, then retry %s.", name);
    } else if (missing == MP_AGENT_READ_CALENDAR && strcmp(name, "calendar_create") == 0) {
        snprintf(output, size, "Call calendar_list, use its result, then retry %s.", name);
    } else if (missing == MP_AGENT_READ_CALENDAR) {
        snprintf(output, size, "Call calendar_list or calendar_get, use its result, then retry %s.", name);
    } else if (missing == MP_AGENT_READ_MISSED) {
        snprintf(output, size, "Call schedule_missed_list, use its result, then retry %s.", name);
    } else if (missing == (MP_AGENT_READ_SCHEDULE | MP_AGENT_READ_TIME)) {
        snprintf(output, size, "Call schedule_list and time_now, use their results, then retry %s.", name);
    } else if (missing == (MP_AGENT_READ_CALENDAR | MP_AGENT_READ_TIME)) {
        snprintf(output, size, "Call calendar_list and time_now, use their results, then retry %s.", name);
    } else {
        snprintf(output, size, "Read the required current state, then retry %s.", name);
    }
}
