// Pbr.ps.hlsl
//
// Cook-Torrance with GGX distribution, Smith height-correlated visibility and
// Schlick Fresnel. Analytic lights only; image-based lighting comes later.
#include "Common.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);
ConstantBuffer<MaterialConstants> g_material : register(b1);

struct PSInput
{
    float4 position      : SV_Position;
    float3 worldPosition : POSITION0;
    float3 normal        : NORMAL0;
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
    const float3 albedo = g_material.baseColour.rgb;
    const float metallic = saturate(g_material.metallic);
    const float roughness = clamp(g_material.roughness, 0.045f, 1.0f);

    const float3 n = normalize(input.normal);
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
    // normal for diffuse and by the reflection vector for specular.
    const float3 skyColour = float3(0.32f, 0.42f, 0.62f);
    const float3 groundColour = float3(0.07f, 0.06f, 0.05f);

    const float3 irradiance = lerp(groundColour, skyColour, saturate(n.y * 0.5f + 0.5f));
    radiance += diffuseColour * irradiance * g_frame.ambient * 6.0f;

    const float3 r = reflect(-v, n);
    const float3 environment = lerp(groundColour, skyColour, saturate(r.y * 0.5f + 0.5f));
    // Rougher surfaces reflect less sharply, so fade the environment with it.
    const float3 fresnelRoughness =
        f0 + (max((1.0f - roughness).xxx, f0) - f0) * pow(saturate(1.0f - nDotV), 5.0f);
    radiance += environment * fresnelRoughness * g_frame.ambient * 6.0f *
                (1.0f - roughness * 0.7f);

    radiance += g_material.emissive;

    const float3 mapped = tonemap(radiance * g_frame.exposure);
    return float4(pow(mapped, 1.0f / 2.2f), g_material.baseColour.a);
}
