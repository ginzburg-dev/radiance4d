// SplatRaster.cs.hlsl
//
// One group per 16x16 tile, one thread per pixel. Each thread walks its tile's
// splat list from the nearest splat outwards, compositing front to back, and
// stops as soon as the accumulated transmittance can no longer change the
// pixel. That early exit is the whole point of binning: the quad rasteriser it
// replaces had to blend every splat that covered the pixel.
//
// The tile's pairs sit in depth order far to near, because they were generated
// from the depth-sorted splats and the tile sort is stable, so the walk runs
// backwards.
#include "SplatTiles.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

// Depth from the mesh pass: a splat behind the geometry is skipped, which is
// what keeps a hybrid scene correct.
Texture2D<float> g_depth  : register(t3);
// The format annotation is for SPIR-V: without it dxc declares the storage
// image as rgba32f and the view would not match.
#ifdef R4D_VULKAN_TARGET
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_target : register(u15);

// One chunk of the tile's list, staged so the 256 threads share the loads.
groupshared float4 s_conic[R4D_SORT_GROUP_SIZE];     // xyz conic, w opacity
groupshared float4 s_screen[R4D_SORT_GROUP_SIZE];    // xy centre, z radius, w depth
groupshared float3 s_colour[R4D_SORT_GROUP_SIZE];
groupshared uint s_done;

[numthreads(R4D_TILE_SIZE, R4D_TILE_SIZE, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID,
          uint localIndex : SV_GroupIndex)
{
    const uint2 grid = splatTileGrid(g_frame.splatViewport);
    const uint tile = group.y * grid.x + group.x;

    const uint2 pixel = group.xy * R4D_TILE_SIZE + local.xy;
    const bool inside = pixel.x < uint(g_frame.splatViewport.x) &&
                        pixel.y < uint(g_frame.splatViewport.y);

    const float meshDepth = inside ? g_depth[pixel] : 0.0f;
    // Pixel centres, matching where the quad rasteriser sampled.
    const float2 centre = float2(pixel) + 0.5f;

    const uint first = g_tileRanges[tile * 2u];
    const uint last = g_tileRanges[tile * 2u + 1u];

    float3 colour = 0.0f.xxx;
    float transmittance = 1.0f;
    bool live = inside && last > first;

    if (localIndex == 0u)
    {
        s_done = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Threads with nothing to do count as finished from the start, or the
    // group would never see itself as saturated.
    if (!live)
    {
        InterlockedAdd(s_done, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // Backwards, in chunks: near to far.
    uint remaining = (last > first) ? (last - first) : 0u;
    while (remaining > 0u)
    {
        const uint take = min(remaining, uint(R4D_SORT_GROUP_SIZE));
        const uint chunkEnd = first + remaining;

        if (localIndex < take)
        {
            const uint splat = g_pairSplatA[chunkEnd - 1u - localIndex];
            const SplatProjection projection = g_projections[splat];
            s_conic[localIndex] = float4(projection.conic, projection.opacity);
            s_screen[localIndex] =
                float4(projection.screen, projection.radius, projection.depth);
            s_colour[localIndex] = projection.colour;
        }
        GroupMemoryBarrierWithGroupSync();

        if (live)
        {
            for (uint i = 0u; i < take; ++i)
            {
                // Behind the mesh: the depth test the quad pass got for free.
                if (s_screen[i].w > meshDepth)
                {
                    continue;
                }

                const float2 d = centre - s_screen[i].xy;
                if (abs(d.x) > s_screen[i].z || abs(d.y) > s_screen[i].z)
                {
                    continue;   // outside the splat's quad
                }

                const float3 conic = s_conic[i].xyz;
                const float power = -0.5f * (conic.x * d.x * d.x + conic.z * d.y * d.y) -
                                    conic.y * d.x * d.y;
                if (power > 0.0f)
                {
                    continue;
                }

                const float alpha = min(0.99f, s_conic[i].w * exp(power));
                if (alpha < 1.0f / 255.0f)
                {
                    continue;
                }

                colour += transmittance * alpha * s_colour[i];
                transmittance *= 1.0f - alpha;
                if (transmittance < 1.0f / 255.0f)
                {
                    live = false;
                    break;
                }
            }

            if (!live)
            {
                InterlockedAdd(s_done, 1u);
            }
        }
        GroupMemoryBarrierWithGroupSync();

        // Every pixel in the tile is saturated, so the rest of the list cannot
        // change the image.
        if (s_done >= uint(R4D_TILE_SIZE * R4D_TILE_SIZE))
        {
            break;
        }

        remaining -= take;
        GroupMemoryBarrierWithGroupSync();
    }

    if (inside)
    {
        g_target[pixel] = float4(colour, 1.0f - transmittance);
    }
}
