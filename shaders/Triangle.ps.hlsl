// Triangle.ps.hlsl
#include "Common.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float3 colour   : COLOR;
};

float4 main(PSInput input) : SV_Target0
{
    return float4(input.colour, 1.0f);
}
