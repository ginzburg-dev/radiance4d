// Shadow.vs.hlsl -- depth-only pass from the key light.
#include "Common.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

float4 main(VSInput input) : SV_Position
{
    return mul(g_frame.lightViewProjection, float4(input.position, 1.0f));
}
