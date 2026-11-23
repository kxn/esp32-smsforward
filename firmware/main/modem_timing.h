#pragma once

/* Milliseconds. Factory call sites and the argument ABI are recorded in
 * docs/08-factory-at.md. Keep per-command deadlines separate from lock wait. */
enum {
    AT_REGISTRATION_MS = 2000,
    AT_OPERATOR_MS = 2000,
    AT_SIM_IDENTITY_MS = 2000,
    AT_SIGNAL_MS = 1000,
    AT_SETUP_MS = 1000,
    AT_HANGUP_MS = 1000,
    AT_SMSC_MS = 1000,
    AT_SMS_SUBMIT_MS = 30000,
};
