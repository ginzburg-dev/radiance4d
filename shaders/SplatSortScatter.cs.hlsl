// SplatSortScatter.cs.hlsl
//
// Writes one tile to its place in the other half of the ping-pong.
//
// A destination is the digit's start in the frame, plus the tile's start within
// that digit, plus the element's rank among the tile's elements that share the
// digit. The rank has to be the element's buffer order for the pass to be
// stable, which is what makes four passes add up to a correct sort of the whole
// key -- and what makes the result identical on every run and on both backends.
//
// The rank comes from a bit matrix: one 256-bit mask per digit saying which
// threads of this tile carry it. A thread's rank is then the population count
// of the bits below its own. That costs one groupshared atomic per element
// instead of the eight ordered split passes a sort-the-tile-first scheme needs.
#include "SplatSort.hlsli"

#define R4D_SORT_MASK_WORDS (R4D_SORT_GROUP_SIZE / 32)

groupshared uint s_mask[R4D_SORT_BUCKETS][R4D_SORT_MASK_WORDS];

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID,
          uint3 thread : SV_DispatchThreadID)
{
    // Uniform across the group, as in the histogram.
    if (group.x * R4D_SORT_GROUP_SIZE >= sortActiveCount())
    {
        return;
    }

    [unroll]
    for (uint word = 0u; word < R4D_SORT_MASK_WORDS; ++word)
    {
        s_mask[local.x][word] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint key = sortReadKey(thread.x);
    const uint value = sortReadValue(thread.x);
    const uint digit = sortDigit(key);

    const uint ownWord = local.x >> 5u;
    const uint ownBit = local.x & 31u;
    InterlockedOr(s_mask[digit][ownWord], 1u << ownBit);
    GroupMemoryBarrierWithGroupSync();

    uint rank = countbits(s_mask[digit][ownWord] & ((1u << ownBit) - 1u));
    for (uint word = 0u; word < ownWord; ++word)
    {
        rank += countbits(s_mask[digit][word]);
    }

    const uint destination =
        g_digitBase[digit] + g_histogram[digit * g_sort.tileCount + group.x] + rank;
    sortWrite(destination, key, value);
}
