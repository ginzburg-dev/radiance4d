// SplatScanAdd.cs.hlsl
//
// Third level of the prefix sum: adds each group's scanned base to the
// per-element offsets, which turns the within-group scan into a global one.
#include "SplatTiles.hlsli"

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    g_tileOffsets[thread.x] += g_groupSums[group.x];
}
