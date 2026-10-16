// SplatBinCount.cs.hlsl
//
// How many tiles each splat covers, indexed by its position in the depth order
// rather than by splat index. Keeping the counts in that order is what makes
// the prefix sum below hand out pair ranges that are already depth-ordered.
#include "SplatTiles.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint position = thread.x;
    if (position >= g_sort.count)
    {
        return;
    }

    if (position >= g_sort.splatCount)
    {
        g_tileCounts[position] = 0u;
        return;
    }

    // The depth sort lands its result in values A, which is the order buffer.
    const SplatProjection splat = g_projections[g_valuesA[position]];

    uint4 span;
    const uint count = splatTileSpan(splat, g_frame.splatViewport, span)
                           ? min(splatTileSpanCount(span), R4D_MAX_TILES_PER_SPLAT)
                           : 0u;
    g_tileCounts[position] = count;
}
