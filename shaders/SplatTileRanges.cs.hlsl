// SplatTileRanges.cs.hlsl
//
// Turns the tile-sorted pairs into one [first, last) range per tile by finding
// the boundaries where the tile id changes. Threads past the real pairs see the
// 0xFFFFFFFF filler and write nothing, so an empty tile keeps the empty range
// the reset left.
#include "SplatTiles.hlsli"

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint pair = thread.x;
    const uint active = sortActiveCount();
    if (pair >= active)
    {
        return;
    }

    const uint tile = g_pairTileA[pair];
    if (tile == 0xFFFFFFFFu)
    {
        return;
    }

    if (pair == 0u || g_pairTileA[pair - 1u] != tile)
    {
        g_tileRanges[tile * 2u] = pair;
    }
    if (pair + 1u == active || g_pairTileA[pair + 1u] != tile)
    {
        g_tileRanges[tile * 2u + 1u] = pair + 1u;
    }
}
