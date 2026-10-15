// Splat.ps.hlsl
#include "Common.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float4 conic    : TEXCOORD0;
    float2 offset   : TEXCOORD1;
    float3 colour   : COLOR0;
};

float4 main(PSInput input) : SV_Target0
{
    const float3 conic = input.conic.xyz;
    const float2 d = input.offset;

    const float power = -0.5f * (conic.x * d.x * d.x + conic.z * d.y * d.y) -
                        conic.y * d.x * d.y;
    if (power > 0.0f)
    {
        discard;
    }

    const float alpha = min(0.99f, input.conic.w * exp(power));
    if (alpha < 1.0f / 255.0f)
    {
        discard;
    }

    // Premultiplied, composited back to front by the draw order.
    return float4(input.colour * alpha, alpha);
}
