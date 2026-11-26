#ifndef PHONE_ORIGIN_H
#define PHONE_ORIGIN_H

/* Describe where an incoming number comes from.

 * The modem hands us whatever string the network sent as CLI, so the caller can
 * be a mainland mobile number, a mainland landline number, an internationally
 * published service number or a number registered abroad. The lookup below
 * recognises those four shapes and returns a short Chinese label: the prefix
 * region for a mobile, the area city for a landline, the organisation behind a
 * service number and the country for an international call.
 *
 * Labels point into a flash-resident table and must not be freed.
 */

typedef enum {
    PHONE_ORIGIN_UNKNOWN = 0,
    PHONE_ORIGIN_MOBILE,
    PHONE_ORIGIN_LANDLINE,
    PHONE_ORIGIN_SERVICE,
    PHONE_ORIGIN_ABROAD,
} phone_origin_kind_t;

/* Returns the kind of number and, when it is known, the label for it.
 * `label` may receive NULL even on a recognised kind, for example when the
 * mobile prefix is not in the allocation table. Forms accepted: bare digits,
 * a leading '+' and the '00' international prefix, with optional separators
 * such as spaces, dashes and brackets. */
phone_origin_kind_t phone_origin_lookup(const char *number, const char **label);

/* Convenience wrapper that returns only the label, or NULL when unknown. */
const char *phone_origin_label(const char *number);

#endif
