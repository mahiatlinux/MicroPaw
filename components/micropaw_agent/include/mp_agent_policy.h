#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    MP_AGENT_READ_MEMORY = 1,
    MP_AGENT_READ_SCHEDULE = 2,
    MP_AGENT_READ_EMAIL = 4,
    MP_AGENT_READ_CALENDAR = 8,
    MP_AGENT_READ_MISSED = 16,
    MP_AGENT_READ_TIME = 32
};

enum {
    MP_AGENT_SLOW_WEB = 1,
    MP_AGENT_SLOW_EMAIL = 2,
    MP_AGENT_SLOW_CALENDAR = 4
};

uint8_t mp_agent_policy_read(const char *name);
uint8_t mp_agent_policy_required(const char *name);
uint8_t mp_agent_policy_slow_stage(const char *name);
const char *mp_agent_policy_fallback(const char *name);
void mp_agent_policy_error(const char *name, uint8_t missing, char *output, size_t size);
