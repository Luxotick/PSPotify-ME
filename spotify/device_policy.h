#ifndef SPOTIFY_DEVICE_POLICY_H
#define SPOTIFY_DEVICE_POLICY_H
#include <stdint.h>

/* Matches the pinned Classic dispatcher audit in docs/EXPERIMENTAL.md.
 * Model identity alone never authorizes patching. Existing family stays T2. */
static inline int device_policy_allows(int model, uint32_t firmware, int table,
                                     uint32_t witness, int mappings_complete)
{
    /* 6.60 is also accepted on the PSP-1000 (confirmed on hardware: same ME
     * image words as expected) and, as a test, on the PSP Go (model 4). */
    const int fw661 = firmware == UINT32_C(0x06060110);
    const int fw660 = firmware == UINT32_C(0x06060010);
    if (!(fw661 || (fw660 && (model == 0 || model == 4))) || !mappings_complete) return 0;
    if (model == 0) return table == 3 && witness == UINT32_C(0x279c1d44);
    /* PSP Go (model 4): test build, accepted only if it reports the T2 image. */
    if (model == 1 || model == 2 || model == 3 || model == 4 || model == 6 || model == 8)
        return table == 2 && witness == UINT32_C(0x279c637c);
    return 0;
}
#endif
