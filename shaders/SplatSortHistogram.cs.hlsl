// SplatSortHistogram.cs.hlsl
//
// Counts this pass's digit over one tile and writes the 256 bins digit-major,
// so that g_histogram[digit * tileCount + tile] is one row per digit.
#include "SplatSort.hlsli"

groupshared uint s_counts[R4D_SORT_BUCKETS];

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID,
          uint3 thread : SV_DispatchThreadID)
{
    // The window is a whole number of groups, so this is uniform: a group is
    // either entirely inside it or has nothing to count.
    if (group.x * R4D_SORT_GROUP_SIZE >= sortActiveCount())
    {
        return;
    }

    s_counts[local.x] = 0u;
    GroupMemoryBarrierWithGroupSync();

    InterlockedAdd(s_counts[sortDigit(sortReadKey(thread.x))], 1u);
    GroupMemoryBarrierWithGroupSync();

    g_histogram[local.x * g_sort.tileCount + group.x] = s_counts[local.x];
}
