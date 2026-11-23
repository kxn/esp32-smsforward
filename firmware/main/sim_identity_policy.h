#pragma once

#include <stdbool.h>
#include <string.h>

enum {
    SIM_IDENTITY_NO_MATCH = 0,
    SIM_IDENTITY_UNIQUE = 1,
    SIM_IDENTITY_AMBIGUOUS = -1,
    SIM_IDENTITY_UNREADABLE = -2,
};

static inline int sim_identity_match(const char *expected,
                                     const char *current[2],
                                     const bool readable[2], int *slot_out)
{
    if (!expected || !expected[0]) return SIM_IDENTITY_NO_MATCH;
    int matches=0, slot=-1;
    for (int i=0;i<2;i++) {
        if (!readable[i] || !current[i] || !current[i][0]) return SIM_IDENTITY_UNREADABLE;
        if (strcmp(expected,current[i])==0) { matches++; slot=i; }
    }
    if (matches>1) return SIM_IDENTITY_AMBIGUOUS;
    if (!matches) return SIM_IDENTITY_NO_MATCH;
    if (slot_out) *slot_out=slot;
    return SIM_IDENTITY_UNIQUE;
}
