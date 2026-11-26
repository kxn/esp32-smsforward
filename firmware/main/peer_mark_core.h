#pragma once

/* Phone-number mark lookup helpers: request URL building and response label
 * extraction.  Pure logic, no ESP-IDF, no cJSON, no heap: host-testable.
 *
 * A "mark" is what online services (Tencent/360/Baidu/Xiaomi/aggregators) attach
 * to a number: 快递, 外卖, 骚扰, 诈骗, 广告推销...  The device can only learn them
 * from a third-party HTTP service, which the owner configures with a URL
 * template; see docs/PEER_MARK.md. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PEER_MARK_NUMBER_MAX 24
#define PEER_MARK_LABEL_MAX  48
#define PEER_MARK_URL_MAX    192
#define PEER_MARK_KEY_MAX    64
/* At most this many distinct labels are joined into one text. */
#define PEER_MARK_LABELS_MAX 2

/* Expand an owner-supplied template into a request URL.
 *
 *   template = "https://api.example.com/mark?key={key}&phone={number}"
 *
 * {number} is mandatory; {key} is replaced when present.  Both substitutions are
 * percent-encoded (numbers keep their leading '+', digits and '.' as-is).  Any
 * other braces are left untouched.
 *
 * Returns the length written (excluding the NUL), or 0 when the template is
 * empty, has no {number}, or the result does not fit. */
size_t peer_mark_build_url(char *out, size_t cap, const char *tmpl,
                           const char *key, const char *number);

/* Extract mark labels from a provider JSON body.
 *
 * Recognised shapes (all in one tolerant pass):
 *   {"results":[{"platform":"360","tags":[{"name":"快递","count":12}]}]}
 *   {"data":{"tags":[{"name":"外卖"}]}}
 *   {"tags":["骚扰","诈骗"]}
 *   {"tag":"广告推销"} / {"mark":"快递"} / {"label":"外卖"} / {"category":"骚扰"}
 *
 * Rules: values of key "name" inside a "tags" array win; the string keys
 * "tag"/"mark"/"label"/"category" are accepted anywhere; duplicates collapse;
 * placeholders such as 未标记/无标记/normal/unknown are ignored; labels longer
 * than PEER_MARK_LABEL_MAX-1 bytes are skipped (never truncated mid-UTF-8).
 * At most PEER_MARK_LABELS_MAX labels are kept, joined with '/'.
 *
 * Returns true when out holds at least one label.  Malformed input yields false
 * and leaves out empty.  len may be shorter than the full body (a truncated
 * response simply yields whatever labels were complete). */
bool peer_mark_parse_labels(const char *json, size_t len, char *out, size_t cap);

/* Join the local address label and the network mark for display:
 *   ("北京", "快递/外卖") -> "北京 · 快递/外卖"
 *   (NULL, "骚扰")        -> "骚扰"
 *   ("北京", NULL)        -> "北京"
 *   (NULL, NULL)          -> ""
 * Truncation never splits a UTF-8 sequence.  Returns the length written. */
size_t peer_mark_join_label(char *out, size_t cap, const char *region, const char *mark);

/* Canonicalise a dial string for the lookup service: keep digits only, then drop
 * a Chinese country code ("0086" or a 13-digit "86...") so every operator form of
 * the same subscriber maps to one query string and one cache key.  Returns the
 * length written, or 0 when there is no usable number or it does not fit. */
size_t peer_mark_normalise_number(const char *in, char *out, size_t cap);
