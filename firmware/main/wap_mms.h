#pragma once

#include <stddef.h>
#include <stdint.h>

#define WAP_MMS_MAX_INPUT (2u * 1024u * 1024u)
#define WAP_MMS_MAX_PARTS 8
#define WAP_MMS_TEXT_MAX 160
#define WAP_MMS_MIME_MAX 64
#define WAP_MMS_CHARSET_MAX 32
#define WAP_MMS_LOCATION_MAX 512
#define WAP_MMS_NAME_MAX 48

typedef enum { WAP_MMS_ENC_NONE = 0, WAP_MMS_ENC_BASE64, WAP_MMS_ENC_QUOTED_PRINTABLE } wap_mms_encoding_t;

typedef enum {
    WAP_MMS_OK = 0,
    WAP_MMS_NOT_MMS = -1,
    WAP_MMS_TRUNCATED = -2,
    WAP_MMS_MALFORMED = -3,
    WAP_MMS_LIMIT = -4,
    WAP_MMS_RETRIEVE_FAILED = -5
} wap_mms_status_t;

typedef struct {
    char sender[WAP_MMS_TEXT_MAX];
    char subject[WAP_MMS_TEXT_MAX];
    char transaction_id[WAP_MMS_TEXT_MAX];
    char content_location[WAP_MMS_LOCATION_MAX];
    uint8_t message_type;
    uint8_t response_status;
    uint8_t has_response_status;
    uint8_t retrieve_status;
    uint8_t has_retrieve_status;
    uint8_t has_message_size;
    uint64_t message_size_bytes;
    uint8_t has_expiry;
    uint8_t expiry_relative;
    uint64_t expiry_seconds; /* relative duration, or absolute Unix seconds */
} wap_mms_info_t;

/* Part data points into the caller's input buffer; it is never copied or executed. */
typedef struct {
    char mime[WAP_MMS_MIME_MAX];
    char charset[WAP_MMS_CHARSET_MAX];
    char filename[WAP_MMS_NAME_MAX];
    wap_mms_encoding_t transfer_encoding;
    const uint8_t *data;
    size_t len;
} wap_mms_part_t;

typedef struct {
    wap_mms_info_t info;
    wap_mms_part_t parts[WAP_MMS_MAX_PARTS];
    size_t part_count;
    size_t ignored_parts;
} wap_mms_retrieve_t;

/* Input is the WSP Push payload after the SMS user-data header was removed. */
wap_mms_status_t wap_mms_parse_push(const uint8_t *data, size_t len, wap_mms_info_t *out);
/* Parse MMS Notification.ind without the outer WSP Push envelope. */
wap_mms_status_t wap_mms_parse_notification(const uint8_t *data, size_t len,
                                            wap_mms_info_t *out);
/* Input is an MMS Encapsulated message (M-Retrieve.conf body included). */
wap_mms_status_t wap_mms_parse_retrieve(const uint8_t *data, size_t len,
                                        wap_mms_retrieve_t *out);
/* Mutable variant decodes Base64/Quoted-Printable parts in place (compacting only that span). */
wap_mms_status_t wap_mms_parse_retrieve_mutable(uint8_t *data, size_t len,
                                                wap_mms_retrieve_t *out);
/* Decode the hex transport used by SMS assembly for binary port-2948 payloads. */
wap_mms_status_t wap_mms_hex_decode(const char *hex, size_t hex_len,
                                    uint8_t *out, size_t out_cap, size_t *out_len);
