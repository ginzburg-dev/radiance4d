// SplatSortScanTotals.cs.hlsl
//
// One group. Replaces each digit's total with where that digit starts in the
// sorted buffer: an exclusive scan of the 256 row totals.
#include "SplatSort.hlsli"

groupshared uint s_scan[R4D_SORT_GROUP_SIZE];

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 local : SV_GroupThreadID)
{
    const uint total = g_digitBase[local.x];

    uint exclusive = 0u;
    uint ignored = 0u;
    R4D_SORT_SCAN_256(s_scan, local.x, total, exclusive, ignored)

    g_digitBase[local.x] = exclusive;
}
