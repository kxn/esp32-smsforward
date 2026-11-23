#ifndef PHONE_REGION_H
#define PHONE_REGION_H

/* Lookup the original prefix allocation for an 11-digit mainland mobile number.
 * Accepts an optional +86, 0086, or 86 prefix. Returns a static UTF-8 location
 * string or NULL for malformed input or an unlisted prefix.
 */
const char *phone_region_lookup(const char *number);

#endif
