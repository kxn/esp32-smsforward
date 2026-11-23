#include "phone_region.h"

#include <stddef.h>
#include <stdint.h>

#include "phone_region_data.inc"

const char *phone_region_lookup(const char *number)
{
    if (!number) return NULL;

    size_t length = 0;
    while (length <= 15 && number[length]) ++length;
    if (length == 15 && number[0] == '0' && number[1] == '0' &&
        number[2] == '8' && number[3] == '6') {
        number += 4;
    } else if (length == 14 && number[0] == '+' && number[1] == '8' && number[2] == '6') {
        number += 3;
    } else if (length == 13 && number[0] == '8' && number[1] == '6') {
        number += 2;
    } else if (length != 11) {
        return NULL;
    }

    uint32_t prefix = 0;
    for (size_t i = 0; i < 11; ++i) {
        char digit = number[i];
        if (digit < '0' || digit > '9') return NULL;
        if (i < 7) prefix = prefix * 10 + (uint32_t)(digit - '0');
    }
    if (prefix < REGION_FIRST_PREFIX ||
        prefix >= REGION_FIRST_PREFIX + REGION_PREFIX_COUNT) {
        return NULL;
    }
    prefix -= REGION_FIRST_PREFIX;
    uint32_t group = prefix / REGION_GROUP_WIDTH;
    uint32_t suffix = prefix % REGION_GROUP_WIDTH;
    uint32_t first = region_groups[group];
    uint32_t low = first;
    uint32_t high = region_groups[group + 1];
    // Find the last boundary at or before the prefix's two-digit suffix.
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if ((uint32_t)(region_boundaries[mid] >> REGION_LABEL_BITS) <= suffix) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low == first) return NULL;
    uint16_t label = region_boundaries[low - 1] & ((1u << REGION_LABEL_BITS) - 1);
    return label ? region_labels + region_label_offsets[label] : NULL;
}
