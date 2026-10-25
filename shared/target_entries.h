#pragma once
#include <stdint.h>

// One selectable identity per exported address. A zero address is unavailable.
// Preferred wins if it aliases another target; Count means no preference.
static inline uint32_t HvUniqueEntryMask(const uint64_t *Addresses, uint32_t Count,
    uint32_t Preferred)
{
    uint32_t i, j, mask = 0;
    if (Addresses == 0 || Count > 32) { return 0; }
    for (i = 0; i < Count; ++i) {
        if (Addresses[i] == 0) { continue; }
        if (Preferred < Count && Addresses[i] == Addresses[Preferred]) {
            if (i == Preferred) { mask |= UINT32_C(1) << i; }
            continue;
        }
        for (j = 0; j < i; ++j) {
            if (Addresses[j] == Addresses[i]) { break; }
        }
        if (j == i) { mask |= UINT32_C(1) << i; }
    }
    return mask;
}
