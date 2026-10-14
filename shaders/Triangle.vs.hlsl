// Triangle.vs.hlsl
#include "Common.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

struct VSInput
{
    float3 position : POSITION;
    float3 colour   : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 colour   : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(g_frame.transform, float4(input.position, 1.0f));
    output.colour = input.colour;
    return output;
}
