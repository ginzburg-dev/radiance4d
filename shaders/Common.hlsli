// Common.hlsli
//
// Matrices are column-major on both sides (dxc -Zpc plus the GLM layout), so
// transform vectors as mul(M, v).

#ifndef R4D_COMMON_HLSLI
#define R4D_COMMON_HLSLI

#define R4D_LIGHT_COUNT 3

struct Light
{
    float3 direction;   // towards the light
    float  intensity;
    float3 colour;
    float  padding;
};

struct FrameConstants
{
    float4x4 viewProjection;
    float3   cameraPosition;
    float    timeSeconds;
    float3   ambient;
    float    exposure;
    Light    lights[R4D_LIGHT_COUNT];
};

struct MaterialConstants
{
    float4 baseColour;
    float3 emissive;
    float  metallic;
    float  roughness;
    float3 padding;
};

#endif
