#include "phone_origin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "phone_origin_data.inc"
#include "phone_region.h"

/* Longest CLI we accept: E.164 allows 15 digits, and a locally formatted
 * mainland landline with separators stays well below this. */
#define ORIGIN_MAX_DIGITS 20

static const char *label_at(uint16_t id)
{
    if (id == 0 || id > ORIGIN_LABEL_COUNT) return NULL;
    return origin_labels + origin_label_offsets[id - 1];
}

static bool find_u16(const uint16_t *codes, const uint16_t *labels, unsigned count,
                     uint16_t key, const char **label)
{
    unsigned low = 0;
    unsigned high = count;
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        if (codes[mid] < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low >= count || codes[low] != key) return false;
    if (label) *label = label_at(labels[low]);
    return true;
}

static bool find_service(uint32_t key, const char **label)
{
    unsigned low = 0;
    unsigned high = ORIGIN_SERVICE_COUNT;
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        if (origin_service_numbers[mid] < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low >= ORIGIN_SERVICE_COUNT || origin_service_numbers[low] != key) return false;
    if (label) *label = label_at(origin_service_labels[low]);
    return true;
}

/* The network sends CLI in one of three shapes: bare digits, a leading '+' or
 * the '00' international prefix. Drop separators, remember which shape it was
 * and copy the digits into `out`. Returns the digit count or 0 when the string
 * is not a number we understand. */
static size_t collect_digits(const char *number, char *out, bool *international, bool *mainland)
{
    *international = false;
    *mainland = false;
    if (!number) return 0;

    size_t length = 0;
    bool plus = false;
    for (const char *p = number; *p; ++p) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            if (length + 1 >= ORIGIN_MAX_DIGITS) return 0;
            out[length++] = c;
            continue;
        }
        if (c == '+') {
            if (length != 0) return 0; /* a plus sign may only open the number */
            plus = true;
        } else if (c != ' ' && c != '-' && c != '(' && c != ')' && c != '.') {
            return 0;
        }
    }
    out[length] = '\0';
    if (length == 0) return 0;

    if (plus) {
        *international = true;
    } else if (length >= 2 && out[0] == '0' && out[1] == '0') {
        *international = true;
        memmove(out, out + 2, length - 1);
        length -= 2;
    }
    if (length == 0) return 0;

    /* A bare 86 prefix is how some networks report a mainland number, and
     * +86 / 0086 are the international spellings of the same thing. */
    if (!*international && length == 13 && out[0] == '8' && out[1] == '6') {
        memmove(out, out + 2, length - 1);
        length -= 2;
    }
    if (*international && length > 11 && out[0] == '8' && out[1] == '6') {
        memmove(out, out + 2, length - 1);
        length -= 2;
        *international = false;
        *mainland = true;
    }
    return length;
}

static uint16_t digits_u16(const char *digits, size_t count)
{
    uint16_t value = 0;
    for (size_t i = 0; i < count; ++i) value = (uint16_t)(value * 10 + (digits[i] - '0'));
    return value;
}

static uint32_t digits_u32(const char *digits, size_t count)
{
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) value = value * 10u + (uint32_t)(digits[i] - '0');
    return value;
}

static phone_origin_kind_t lookup_abroad(const char *digits, size_t length, const char **label)
{
    /* Country calling codes are one to three digits and no code is a prefix of
     * another, so the longest match is also the only match. */
    for (int width = 3; width >= 1; --width) {
        if (length < (size_t)width) continue;
        if (find_u16(origin_country_codes, origin_country_labels, ORIGIN_COUNTRY_COUNT,
                     digits_u16(digits, (size_t)width), label)) {
            return PHONE_ORIGIN_ABROAD;
        }
    }
    return PHONE_ORIGIN_ABROAD;
}

static bool lookup_area(const char *digits, size_t length, const char **label)
{
    /* Area codes are '0' plus two or three digits. Try the longer form first;
     * the two-digit trunk codes (010 and 02x) cannot be shadowed by it because
     * every area code of that length starts with 3..9, so '0102' and '0208'
     * never match an entry. */
    if (length >= 4 &&
        find_u16(origin_area_codes, origin_area_labels, ORIGIN_AREA_COUNT,
                 digits_u16(digits, 4), label)) {
        return true;
    }
    if (length >= 3 &&
        find_u16(origin_area_codes, origin_area_labels, ORIGIN_AREA_COUNT,
                 digits_u16(digits, 3), label)) {
        return true;
    }
    return false;
}

static phone_origin_kind_t lookup_domestic(const char *digits, size_t length, const char **label)
{
    /* Mainland mobile numbers are handed to the prefix table unchanged so both
     * lookups keep using the same normalisation rules. */
    if (length == 11 && digits[0] == '1') {
        const char *region = phone_region_lookup(digits);
        if (label) *label = region;
        return PHONE_ORIGIN_MOBILE;
    }

    if (length <= 5 && find_service(digits_u32(digits, length), label)) {
        return PHONE_ORIGIN_SERVICE;
    }

    /* 400 and 800 numbers are rented by companies; the specific owner is not
     * published, so all we can say is that it is a corporate service line. */
    if (length == 10 && ((digits[0] == '4' && digits[1] == '0' && digits[2] == '0') ||
                         (digits[0] == '8' && digits[1] == '0' && digits[2] == '0'))) {
        if (label) *label = label_at(ORIGIN_BULK_LABEL);
        return PHONE_ORIGIN_SERVICE;
    }

    if (digits[0] == '0' && length >= 7) {
        (void)lookup_area(digits, length, label);
        return PHONE_ORIGIN_LANDLINE;
    }

    return PHONE_ORIGIN_UNKNOWN;
}

phone_origin_kind_t phone_origin_lookup(const char *number, const char **label)
{
    if (label) *label = NULL;

    char digits[ORIGIN_MAX_DIGITS];
    bool international = false;
    bool mainland = false;
    size_t length = collect_digits(number, digits, &international, &mainland);
    if (length == 0) return PHONE_ORIGIN_UNKNOWN;
    if (international) return lookup_abroad(digits, length, label);

    phone_origin_kind_t kind = lookup_domestic(digits, length, label);
    if (kind != PHONE_ORIGIN_UNKNOWN || !mainland) return kind;

    /* The international spelling of a mainland number drops the trunk '0':
     * +86 10 … is 010… at home and +86 755 … is 0755…. Restore it and retry,
     * which is the only way to label an internationally formatted landline. */
    if (length + 2 > ORIGIN_MAX_DIGITS) return PHONE_ORIGIN_UNKNOWN;
    char restored[ORIGIN_MAX_DIGITS];
    restored[0] = '0';
    memcpy(restored + 1, digits, length);
    restored[length + 1] = '\0';
    return lookup_domestic(restored, length + 1, label);
}

const char *phone_origin_label(const char *number)
{
    const char *label = NULL;
    (void)phone_origin_lookup(number, &label);
    return label;
}
