// Splat.vs.hlsl
//
// Projects a 3D Gaussian to a screen-space 2D Gaussian and emits a quad that
// covers it. One instance per splat, four vertices generated from SV_VertexID,
// so there is no vertex buffer.
#include "Common.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

struct Gaussian
{
    float3 position;
    float  opacity;
    float3 scale;
    float  padding0;
    float4 rotation;   // quaternion, xyzw, matching the glm storage order
    float3 colour;
    float  padding1;
};

StructuredBuffer<Gaussian> g_gaussians : register(t0);
StructuredBuffer<uint>     g_order     : register(t1);

struct VSOutput
{
    float4 position : SV_Position;
    float4 conic    : TEXCOORD0;   // xyz conic, w opacity
    float2 offset   : TEXCOORD1;   // pixels from the centre
    float3 colour   : COLOR0;
};

float3x3 quaternionToMatrix(float4 q)
{
    const float w = q.w, x = q.x, y = q.y, z = q.z;
    return float3x3(
        1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z),        2.0f * (x * z + w * y),
        2.0f * (x * y + w * z),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x),
        2.0f * (x * z - w * y),        2.0f * (y * z + w * x),        1.0f - 2.0f * (x * x + y * y));
}

VSOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    VSOutput output;
    output.position = float4(0.0f, 0.0f, 2.0f, 1.0f);   // offscreen by default
    output.conic = 0.0f.xxxx;
    output.offset = 0.0f.xx;
    output.colour = 0.0f.xxx;

    const Gaussian g = g_gaussians[g_order[instanceId]];

    const float4 viewPosition = mul(g_frame.view, float4(g.position, 1.0f));
    if (viewPosition.z < g_frame.splatNearPlane)
    {
        return output;   // behind the camera
    }

    // Sigma = R S S^T R^T, built in world space.
    const float3x3 rotation = quaternionToMatrix(g.rotation);
    const float3x3 scale = float3x3(g.scale.x, 0.0f, 0.0f,
                                    0.0f, g.scale.y, 0.0f,
                                    0.0f, 0.0f, g.scale.z);
    const float3x3 m = mul(rotation, scale);
    const float3x3 sigma = mul(m, transpose(m));

    // Jacobian of the perspective projection at this point, in view space.
    const float focalX = g_frame.splatFocal.x;
    const float focalY = g_frame.splatFocal.y;
    const float invZ = 1.0f / viewPosition.z;
    const float invZ2 = invZ * invZ;

    // Screen y points down, so the second row carries the opposite sign to the
    // first. Getting that wrong mirrors every anisotropic splat.
    const float3x3 jacobian = float3x3(
        focalX * invZ,  0.0f,          -focalX * viewPosition.x * invZ2,
        0.0f,          -focalY * invZ,   focalY * viewPosition.y * invZ2,
        0.0f,           0.0f,            0.0f);

    const float3x3 viewRotation = (float3x3)g_frame.view;
    const float3x3 t = mul(jacobian, viewRotation);
    const float3x3 covariance = mul(mul(t, sigma), transpose(t));

    // A small dilation keeps very thin splats from falling between pixels.
    float a = covariance._11 + 0.3f;
    float b = covariance._12;
    float c = covariance._22 + 0.3f;

    const float determinant = a * c - b * b;
    if (determinant <= 0.0f)
    {
        return output;
    }

    const float invDeterminant = 1.0f / determinant;
    const float3 conic = float3(c * invDeterminant, -b * invDeterminant, a * invDeterminant);

    // Radius from the larger eigenvalue, at three standard deviations.
    const float mid = 0.5f * (a + c);
    const float offset = sqrt(max(mid * mid - determinant, 0.0001f));
    const float radius = 3.0f * sqrt(max(mid + offset, 0.0001f));

    const float4 clip = mul(g_frame.viewProjection, float4(g.position, 1.0f));
    if (clip.w <= 0.0f)
    {
        return output;
    }

    const float2 ndc = clip.xy / clip.w;
    const float2 screen = (ndc * float2(0.5f, -0.5f) + 0.5f) * g_frame.splatViewport;

    // Quad corners: 0,1,2,3 as a triangle strip.
    const float2 corner = float2((vertexId & 1) ? 1.0f : -1.0f,
                                 (vertexId & 2) ? 1.0f : -1.0f);
    const float2 pixel = screen + corner * radius;

    const float2 quadNdc = (pixel / g_frame.splatViewport - 0.5f) * float2(2.0f, -2.0f);

    output.position = float4(quadNdc, clip.z / clip.w, 1.0f);
    output.conic = float4(conic, g.opacity);
    output.offset = corner * radius;
    output.colour = g.colour;
    return output;
}
