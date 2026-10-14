// Common.hlsli -- shared shader declarations.
//
// Matrix convention: shaders are compiled with dxc -Zpc (column-major packing),
// which matches GLM's in-memory layout, so a glm::mat4 uploads to a float4x4
// with no transpose. Transform vectors as mul(M, v), never mul(v, M).

#ifndef R4D_COMMON_HLSLI
#define R4D_COMMON_HLSLI

struct FrameConstants
{
    float4x4 transform;      // full model-view-projection
    float    timeSeconds;
    float3   padding;
};

#endif // R4D_COMMON_HLSLI
