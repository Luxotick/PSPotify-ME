#ifndef SPOTIFY_DEVICE_POLICY_H
#define SPOTIFY_DEVICE_POLICY_H
#include <stdint.h>

/* Matches the pinned Classic dispatcher audit in docs/EXPERIMENTAL.md.
 * Model identity alone never authorizes patching. Existing family stays T2. */
static inline int device_policy_allows(int model, uint32_t firmware, int table,
                                     uint32_t witness, int mappings_complete)
{
    /* Test build: 6.60 is also accepted on the PSP-1000, where the ME image
     * witness matched the 6.61 value on hardware. Not yet proven safe. */
    const int fw661 = firmware == UINT32_C(0x06060110);
    const int fw660 = firmware == UINT32_C(0x06060010);
    if (!(fw661 || (fw660 && model == 0)) || !mappings_complete) return 0;
    if (model == 0) return table == 3 && witness == UINT32_C(0x279c1d44);
    if (model == 1 || model == 2 || model == 3 || model == 6 || model == 8)
        return table == 2 && witness == UINT32_C(0x279c637c);
    return 0;
}
#endif
