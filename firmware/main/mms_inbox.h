#pragma once

#include "modem.h"

/* Called by the SMS worker after the original notification and SIM binding
 * are durable. SIM storage retains work while another transfer is active. */
void mms_inbox_schedule(modem_t *m, const char *url, const char *parent,
                        const char *record_id, uint64_t key, bool (*commit)(uint64_t));
