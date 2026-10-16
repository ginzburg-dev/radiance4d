// Splat.vs.hlsl
//
// Expands one quad per visible splat from SV_VertexID, so there is no vertex
// buffer. The projection itself happened once per splat in SplatCull.cs.hlsl;
// this shader only reads the conic it left, which is four times less work than
// rebuilding the covariance at every corner.
#include "Splat.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

StructuredBuffer<SplatProjection> g_projections : register(t0);
// Written by the compute sort, back to front, earlier in the same frame. Only
// the first instanceCount entries are visible, and the draw is sized to that.
StructuredBuffer<uint>            g_order       : register(t1);

struct VSOutput
{
    float4 position : SV_Position;
    float4 conic    : TEXCOORD0;   // xyz conic, w opacity
    float2 offset   : TEXCOORD1;   // pixels from the centre
    float3 colour   : COLOR0;
};

VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const SplatProjection splat = g_projections[g_order[instanceId]];

    // Quad corners: 0,1,2,3 as a triangle strip.
    const float2 corner = float2((vertexId & 1) ? 1.0f : -1.0f,
                                 (vertexId & 2) ? 1.0f : -1.0f);
    const float2 offset = corner * splat.radius;
    const float2 pixel = splat.screen + offset;
    const float2 quadNdc = (pixel / g_frame.splatViewport - 0.5f) * float2(2.0f, -2.0f);

    VSOutput output;
    output.position = float4(quadNdc, splat.depth, 1.0f);
    output.conic = float4(splat.conic, splat.opacity);
    output.offset = offset;
    output.colour = splat.colour;
    return output;
}
