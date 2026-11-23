#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MMS_BODY_MAX (2u * 1024u * 1024u)
#define MMS_HTTP_OVERHEAD_MAX (64u * 1024u)

typedef struct {
    const char *apn;
    const char *host;
    const char *proxy;
    unsigned port;
} mms_profile_t;

/* Parse actual CIMI and CEREG? replies. Unknown or ambiguous input is denied. */
const mms_profile_t *mms_profile_from_imsi(const char *response);
bool mms_home_registered(const char *cereg_response);
bool mms_serving_home(const mms_profile_t *profile, const char *cops_response);
int mms_http_request(const mms_profile_t *profile, const char *url, char *out, size_t cap);

/* -1 invalid, 0 more bytes needed (wire untouched), 1 body at wire[0..body_len). */
int mms_http_decode(uint8_t *wire, size_t len, bool eof, size_t *body_len);
