// SplatScanGroups.cs.hlsl
//
// First level of the prefix sum over the per-splat tile counts: an exclusive
// scan inside each group, with the group's total left for the next kernel.
#include "SplatTiles.hlsli"

groupshared uint s_scan[R4D_SORT_GROUP_SIZE];

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID, uint3 group : SV_GroupID,
          uint3 local : SV_GroupThreadID)
{
    const uint count = g_tileCounts[thread.x];

    uint exclusive = 0u;
    uint total = 0u;
    R4D_SORT_SCAN_256(s_scan, local.x, count, exclusive, total);

    g_tileOffsets[thread.x] = exclusive;
    if (local.x == 0u)
    {
        g_groupSums[group.x] = total;
    }
}
