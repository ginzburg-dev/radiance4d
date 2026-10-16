// SplatComposite.vs.hlsl
//
// A fullscreen triangle from SV_VertexID, with no vertex buffer and no
// constants: three vertices that cover the viewport.
float4 main(uint vertexId : SV_VertexID) : SV_Position
{
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
