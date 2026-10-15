// Pbr.ps.hlsl
//
// Cook-Torrance with GGX distribution, Smith height-correlated visibility and
// Schlick Fresnel. Analytic lights only; image-based lighting comes later.
#include "Common.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);
ConstantBuffer<MaterialConstants> g_material : register(b1);

Texture2D g_baseColour        : register(t0);
Texture2D g_metallicRoughness : register(t1);
Texture2D g_normal            : register(t2);
SamplerState g_sampler        : register(s0);

struct PSInput
{
    float4 position      : SV_Position;
    float3 worldPosition : POSITION0;
    float3 normal        : NORMAL0;
    float4 tangent       : TANGENT0;
    float2 uv            : TEXCOORD0;
};

static const float kPi = 3.14159265f;

float distributionGGX(float nDotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * d * d, 1e-7f);
}

float visibilitySmith(float nDotV, float nDotL, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float v = nDotL * sqrt(nDotV * nDotV * (1.0f - a2) + a2);
    const float l = nDotV * sqrt(nDotL * nDotL * (1.0f - a2) + a2);
    return 0.5f / max(v + l, 1e-7f);
}

float3 fresnelSchlick(float3 f0, float vDotH)
{
    return f0 + (1.0f - f0) * pow(saturate(1.0f - vDotH), 5.0f);
}

// Karis split-sum approximation: an analytic stand-in for the prefiltered
// environment BRDF lookup, so metals respond correctly without a LUT.
float3 environmentBRDF(float3 f0, float roughness, float nDotV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * nDotV)) * r.x + r.y;
    const float2 ab = float2(-1.04f, 1.04f) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float3 tonemap(float3 colour)
{
    // ACES approximation.
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((colour * (a * colour + b)) / (colour * (c * colour + d) + e));
}

float4 main(PSInput input) : SV_Target0
{
    const float4 baseSample = g_baseColour.Sample(g_sampler, input.uv);
    const float4 base = g_material.baseColour * baseSample;
    const float3 albedo = base.rgb;

    // glTF packs roughness in green and metallic in blue.
    const float3 mrSample = g_metallicRoughness.Sample(g_sampler, input.uv).rgb;
    const float metallic = saturate(g_material.metallic * mrSample.b);
    const float roughness = clamp(g_material.roughness * mrSample.g, 0.045f, 1.0f);

    float3 n = normalize(input.normal);
    {
        const float3 t = normalize(input.tangent.xyz - n * dot(n, input.tangent.xyz));
        const float3 b = cross(n, t) * input.tangent.w;
        const float3 sampled = g_normal.Sample(g_sampler, input.uv).xyz * 2.0f - 1.0f;
        n = normalize(mul(sampled, float3x3(t, b, n)));
    }
    const float3 v = normalize(g_frame.cameraPosition - input.worldPosition);
    const float nDotV = saturate(dot(n, v)) + 1e-5f;

    const float3 f0 = lerp(0.04f.xxx, albedo, metallic);
    const float3 diffuseColour = albedo * (1.0f - metallic);

    float3 radiance = 0.0f.xxx;

    [unroll]
    for (int i = 0; i < R4D_LIGHT_COUNT; ++i)
    {
        const float3 l = normalize(g_frame.lights[i].direction);
        const float nDotL = saturate(dot(n, l));
        if (nDotL <= 0.0f)
        {
            continue;
        }

        const float3 h = normalize(l + v);
        const float nDotH = saturate(dot(n, h));
        const float vDotH = saturate(dot(v, h));

        const float d = distributionGGX(nDotH, roughness);
        const float vis = visibilitySmith(nDotV, nDotL, roughness);
        const float3 f = fresnelSchlick(f0, vDotH);

        const float3 specular = d * vis * f;
        const float3 diffuse = (1.0f - f) * diffuseColour / kPi;

        radiance += (diffuse + specular) * g_frame.lights[i].colour *
                    g_frame.lights[i].intensity * nDotL;
    }

    // Stand-in for image-based lighting: a vertical sky gradient sampled by the
    // normal for irradiance and by the reflection vector for specular.
    const float3 skyColour = float3(0.34f, 0.44f, 0.66f);
    const float3 groundColour = float3(0.06f, 0.055f, 0.05f);

    const float3 irradiance = lerp(groundColour, skyColour, saturate(n.y * 0.5f + 0.5f));
    radiance += diffuseColour * irradiance * g_frame.ambient;

    const float3 r = reflect(-v, n);
    const float3 environment = lerp(groundColour, skyColour, saturate(r.y * 0.5f + 0.5f));
    radiance += environment * environmentBRDF(f0, roughness, nDotV) * g_frame.ambient;

    radiance += g_material.emissive;

    const float3 mapped = tonemap(radiance * g_frame.exposure);
    return float4(pow(mapped, 1.0f / 2.2f), base.a);
}
