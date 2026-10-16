// SplatTileRangesReset.cs.hlsl
//
// Empties every tile's range before the boundaries are found, so a tile that no
// splat touches this frame does not keep last frame's list.
#include "SplatTiles.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint2 grid = splatTileGrid(g_frame.splatViewport);
    if (thread.x >= grid.x * grid.y)
    {
        return;
    }

    g_tileRanges[thread.x * 2u] = 0u;
    g_tileRanges[thread.x * 2u + 1u] = 0u;
}
