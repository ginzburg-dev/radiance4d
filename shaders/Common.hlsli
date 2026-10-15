// Common.hlsli
//
// Matrices are column-major on both sides (dxc -Zpc plus the GLM layout), so
// transform vectors as mul(M, v).

#ifndef R4D_COMMON_HLSLI
#define R4D_COMMON_HLSLI

#define R4D_LIGHT_COUNT 3

// Spherical-harmonic coefficients per splat. Must match kShCoefficients in
// GaussianCloud.hpp, which asserts the resulting struct size.
#define R4D_SH_COEFFICIENTS 16

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
    float4x4 lightViewProjection;
    float4x4 view;
    float3   cameraPosition;
    float    timeSeconds;
    float3   ambient;
    float    exposure;
    float    shadowTexelSize;
    float3   shadowPadding;
    float2   splatViewport;
    float2   splatFocal;
    float    splatNearPlane;
    float3   splatPadding;
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
