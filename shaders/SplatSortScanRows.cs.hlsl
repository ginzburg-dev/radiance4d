// SplatSortScanRows.cs.hlsl
//
// One group per digit. Turns that digit's row of per-tile counts into an
// exclusive scan in place, and leaves the row's total in g_digitBase, which the
// next kernel scans. A single group walks the row with a carry, so the result
// does not depend on how the groups happen to be scheduled.
#include "SplatSort.hlsli"

groupshared uint s_scan[R4D_SORT_GROUP_SIZE];
groupshared uint s_carry;

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    // The row is addressed at the full stride, so that the scatter finds it
    // where it expects, but only the tiles the histogram wrote are scanned.
    const uint row = group.x * g_sort.tileCount;
    const uint tiles = sortActiveCount() / R4D_SORT_GROUP_SIZE;

    if (local.x == 0u)
    {
        s_carry = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint base = 0u; base < tiles; base += R4D_SORT_GROUP_SIZE)
    {
        const uint tile = base + local.x;
        const bool live = tile < tiles;
        const uint count = live ? g_histogram[row + tile] : 0u;

        uint exclusive = 0u;
        uint total = 0u;
        R4D_SORT_SCAN_256(s_scan, local.x, count, exclusive, total)

        if (live)
        {
            g_histogram[row + tile] = s_carry + exclusive;
        }
        GroupMemoryBarrierWithGroupSync();

        if (local.x == 0u)
        {
            s_carry += total;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (local.x == 0u)
    {
        g_digitBase[group.x] = s_carry;
    }
}
