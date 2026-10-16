// SplatScanGroupSums.cs.hlsl
//
// Second level of the prefix sum. One group walks the per-group totals with a
// carry, so the result does not depend on how groups are scheduled, and leaves
// the grand total in the slot past the end.
#include "SplatTiles.hlsli"

groupshared uint s_scan[R4D_SORT_GROUP_SIZE];
groupshared uint s_carry;

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 local : SV_GroupThreadID)
{
    const uint groups = g_sort.count / R4D_SORT_GROUP_SIZE;

    if (local.x == 0u)
    {
        s_carry = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint base = 0u; base < groups; base += R4D_SORT_GROUP_SIZE)
    {
        const uint index = base + local.x;
        const bool live = index < groups;
        const uint value = live ? g_groupSums[index] : 0u;

        uint exclusive = 0u;
        uint total = 0u;
        R4D_SORT_SCAN_256(s_scan, local.x, value, exclusive, total);

        if (live)
        {
            g_groupSums[index] = s_carry + exclusive;
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
        g_groupSums[groups] = s_carry;   // the pair total, for the bin pass
    }
}
