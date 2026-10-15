// Pbr.vs.hlsl
#include "Common.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct VSOutput
{
    float4 position      : SV_Position;
    float3 worldPosition : POSITION0;
    float3 normal        : NORMAL0;
    float4 tangent       : TANGENT0;
    float2 uv            : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.worldPosition = input.position;
    output.position = mul(g_frame.viewProjection, float4(input.position, 1.0f));
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.uv = input.uv;
    return output;
}
