// Splat.hlsli
//
// The splat record, shared by the rasterising vertex shader and the compute
// sort. One declaration means the 240-byte POD in GaussianCloud.hpp cannot
// drift between the two.

#ifndef R4D_SPLAT_HLSLI
#define R4D_SPLAT_HLSLI

#include "Common.hlsli"

struct Gaussian
{
    float3 position;
    float  opacity;
    float3 scale;
    float  padding0;
    float4 rotation;   // quaternion, xyzw, matching the glm storage order
    // Coefficient-major: band k, channel c at sh[k * 3 + c].
    float  sh[R4D_SH_COEFFICIENTS * 3];
};

// What the cull pass leaves for the rasteriser: the splat already projected to
// a screen-space conic, so the vertex shader reads it instead of rebuilding the
// covariance once per corner.
struct SplatProjection
{
    float2 screen;    // centre, in pixels
    float  radius;    // half the quad, in pixels
    float  depth;     // clip z/w, tested against the mesh pass
    float3 conic;     // inverse 2D covariance
    float  opacity;
    float3 colour;    // spherical harmonics already evaluated
    float  padding;
};

#endif
