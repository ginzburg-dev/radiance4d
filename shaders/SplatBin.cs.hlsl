// SplatBin.cs.hlsl
//
// Writes the (tile, splat) pairs. One thread per position in the depth order,
// so the pairs come out in depth order and a stable sort by tile keeps them
// that way inside each tile.
//
// Threads past the splats fill the rest of the capacity with the largest tile
// id, which sorts behind every real pair and is ignored when the ranges are
// found. That is also what clears the previous frame's pairs.
#include "SplatTiles.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint position = thread.x;

    // The dispatch covers the pair capacity, which is the larger of the two.
    if (position < g_sort.pairCapacity)
    {
        const uint total = g_groupSums[g_sort.count / R4D_SORT_GROUP_SIZE];
        if (position >= total)
        {
            g_pairTileA[position] = 0xFFFFFFFFu;
            g_pairSplatA[position] = 0u;
        }
    }

    if (position >= g_sort.splatCount)
    {
        return;
    }

    const uint splat = g_valuesA[position];
    const SplatProjection projection = g_projections[splat];

    uint4 span;
    if (!splatTileSpan(projection, g_frame.splatViewport, span))
    {
        return;
    }

    const uint2 grid = splatTileGrid(g_frame.splatViewport);
    const uint limit = min(splatTileSpanCount(span), R4D_MAX_TILES_PER_SPLAT);

    uint slot = g_tileOffsets[position];
    uint written = 0u;

    for (uint y = span.y; y <= span.w && written < limit; ++y)
    {
        for (uint x = span.x; x <= span.z && written < limit; ++x)
        {
            if (slot < g_sort.pairCapacity)
            {
                g_pairTileA[slot] = y * grid.x + x;
                g_pairSplatA[slot] = splat;
            }
            ++slot;
            ++written;
        }
    }
}
