#pragma once

#include <stddef.h>

/* Validate UTF-8 and count UTF-16 code units required by the UCS2 PDU encoder. */
int sms_text_ucs2_units(const char *text);
/* Decode ordinary assembled 8-bit text in place. Preserve valid UTF-8 and
 * render other bytes as Latin-1; embedded NULs must not truncate the rest. */
int sms_text_display_8bit(char *bytes, size_t len, size_t cap);
/* Split on codepoint boundaries. Returns units written, 0 at end, or -1 on invalid UTF-8. */
int sms_text_next_chunk(const char *text, char *out, size_t out_size, int max_units);
/* Count actual chunks using the same splitter as sms_send; >8 means unsupported. */
int sms_text_segment_count(const char *text);
