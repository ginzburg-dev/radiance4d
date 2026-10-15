// SplatSortKey.cs.hlsl
//
// Builds the (depth key, splat index) pairs the radix sort consumes. One thread
// per padded element; the padding carries the largest key so that it sorts past
// every real splat and the draw, which reads only the first splatCount entries,
// never sees it.
#include "Splat.hlsli"
#include "SplatSort.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

StructuredBuffer<Gaussian> g_gaussians : register(t0);

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x;
    if (index >= g_sort.count)
    {
        return;
    }

    if (index >= g_sort.splatCount)
    {
        g_keysA[index] = 0xFFFFFFFFu;
        g_valuesA[index] = 0u;
        return;
    }

    // The same view-space z the vertex shader projects with, so the order and
    // the geometry cannot disagree about which splat is in front.
    const float depth = mul(g_frame.view, float4(g_gaussians[index].position, 1.0f)).z;
    g_keysA[index] = splatDepthKey(depth);
    g_valuesA[index] = index;
}
