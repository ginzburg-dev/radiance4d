// SplatSort.hlsli
//
// Shared state for the GPU radix sort that orders the splats back to front.
//
// Four passes over an 8-bit digit of a 32-bit key. The key carries the whole
// float depth, so the ordering is exact rather than quantised, and the sort is
// stable, so equal depths keep their buffer order on every run and on both
// backends.
//
// Layout of a pass:
//   histogram   one group per tile, a 256-bin count written DIGIT-MAJOR
//   scan rows   one group per digit, exclusive scan along that digit's row
//   scan totals one group, exclusive scan of the 256 row totals
//   scatter     one group per tile, stable write to the other ping-pong half
//
// Digit-major histograms are what make the scan cheap: exclusive along a row
// gives a tile its offset within a digit, and the scanned row totals give the
// digit its offset within the frame, so the scatter just adds the two.

#ifndef R4D_SPLATSORT_HLSLI
#define R4D_SPLATSORT_HLSLI

#include "Splat.hlsli"

// One element per thread. Tiles of exactly this size let every kernel but the
// key build run without bounds checks: the buffers are padded up to a whole
// number of tiles and the padding carries the largest possible key.
#define R4D_SORT_GROUP_SIZE 256
#define R4D_SORT_DIGIT_BITS 8
#define R4D_SORT_BUCKETS    (1u << R4D_SORT_DIGIT_BITS)
#define R4D_SORT_PASSES     (32 / R4D_SORT_DIGIT_BITS)

// Eight scalars, so the layout is the same whether this arrives as D3D12 root
// constants or as a Vulkan uniform buffer, with no vector straddling a 16-byte
// row. Nothing here changes with the camera: a pass's constants are fixed once
// the splat count is known.
struct SortConstants
{
    uint count;       // padded: tileCount * R4D_SORT_GROUP_SIZE
    uint splatCount;  // real splats; the rest of count is padding
    uint tileCount;
    uint shift;       // bit offset of this pass's digit
    uint sourceIsB;   // which half of the ping-pong this pass reads
    uint padding0;
    uint padding1;
    uint padding2;
};

ConstantBuffer<SortConstants> g_sort : register(b1);

// Both halves of the ping-pong are bound at once and selected by a value that
// is uniform across the dispatch, which is cheaper than rewriting descriptors
// between passes and keeps one descriptor set for the whole sort.
RWStructuredBuffer<uint> g_keysA     : register(u0);
RWStructuredBuffer<uint> g_keysB     : register(u1);
RWStructuredBuffer<uint> g_valuesA   : register(u2);
RWStructuredBuffer<uint> g_valuesB   : register(u3);
RWStructuredBuffer<uint> g_histogram : register(u4);
RWStructuredBuffer<uint> g_digitBase : register(u5);

// Written by the cull pass, read by the draw.
RWStructuredBuffer<SplatProjection> g_projections : register(u6);

uint sortReadKey(uint i)
{
    return g_sort.sourceIsB != 0 ? g_keysB[i] : g_keysA[i];
}

uint sortReadValue(uint i)
{
    return g_sort.sourceIsB != 0 ? g_valuesB[i] : g_valuesA[i];
}

void sortWrite(uint i, uint key, uint value)
{
    if (g_sort.sourceIsB != 0)
    {
        g_keysA[i] = key;
        g_valuesA[i] = value;
    }
    else
    {
        g_keysB[i] = key;
        g_valuesB[i] = value;
    }
}

uint sortDigit(uint key)
{
    return (key >> g_sort.shift) & (R4D_SORT_BUCKETS - 1u);
}

// An exclusive scan of 256 values held one per thread, Hillis-Steele in
// groupshared. Returns the running total through s_total as well, since every
// caller needs it. The array is the caller's so that a kernel which also needs
// groupshared for something else does not pay for two.
#define R4D_SORT_SCAN_256(s_array, threadId, value, exclusive, total)                   \
    {                                                                                   \
        s_array[threadId] = value;                                                      \
        GroupMemoryBarrierWithGroupSync();                                              \
        for (uint _offset = 1; _offset < R4D_SORT_GROUP_SIZE; _offset <<= 1)             \
        {                                                                               \
            const uint _addend = (threadId >= _offset) ? s_array[threadId - _offset] : 0u; \
            GroupMemoryBarrierWithGroupSync();                                          \
            s_array[threadId] += _addend;                                               \
            GroupMemoryBarrierWithGroupSync();                                          \
        }                                                                               \
        exclusive = s_array[threadId] - value;                                          \
        total = s_array[R4D_SORT_GROUP_SIZE - 1];                                       \
        GroupMemoryBarrierWithGroupSync();                                              \
    }

// Maps a float onto a uint whose unsigned ordering is the reverse of the
// float's, so that an ascending sort puts the farthest splat first. The sign
// bit is flipped for positives and every bit for negatives, which is exact: no
// depth range is assumed and no precision is dropped.
uint splatDepthKey(float depth)
{
    const uint bits = asuint(depth);
    const uint ordered = (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
    return ~ordered;
}

#endif
