// SplatComposite.ps.hlsl
//
// Puts the tile rasteriser's result over the mesh image. The texture already
// holds premultiplied colour and coverage, so the blend state is the same
// ONE / INV_SRC_ALPHA the quad pass used.
Texture2D<float4> g_splats : register(t0);

float4 main(float4 position : SV_Position) : SV_Target0
{
    return g_splats.Load(int3(int2(position.xy), 0));
}
