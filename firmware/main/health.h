#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "modem.h"

/* Why a modem was hardware-reset; exposed on the status page. */
typedef enum {
    MODEM_RESET_REASON_AT = 1,        /* AT probe stopped answering */
    MODEM_RESET_REASON_SMS = 2,       /* AT alive but CPMS/CNMI path stuck */
    MODEM_RESET_REASON_MANUAL = 3,    /* owner pressed reset */
    MODEM_RESET_REASON_SCHEDULED = 4, /* periodic maintenance */
} modem_reset_reason_t;

typedef struct {
    bool at_ok;
    bool cpms_ok;
    bool cnmi_ok;
    unsigned probes;
    unsigned at_fail_streak;
    unsigned sms_fail_streak;
    unsigned resets;
    unsigned repairs;
    int64_t last_probe_ms;   /* 0 = never probed this boot */
    int64_t last_reset_ms;   /* 0 = never reset this boot */
    uint8_t last_reset_reason;
} modem_health_status_t;

void health_start(modem_t *modems);
void health_sms_blink(void);
void health_error_blink(void);
/* slot is 0-based; -1 requests both.  False when the request was rejected. */
bool health_request_reset(int slot);
/* Probe the modems on the next health tick instead of waiting for the 5 min timer. */
void health_request_probe(void);
void health_get_status(int slot, modem_health_status_t *out);
