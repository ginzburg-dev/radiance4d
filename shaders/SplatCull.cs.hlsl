// SplatCull.cs.hlsl
//
// Projects every splat once and decides whether it can affect the frame. What
// survives is recorded as a screen-space conic the vertex shader only has to
// read, and counted into the indirect draw's instance count.
//
// This also builds the (depth key, splat index) pairs the radix sort consumes,
// since the view depth is already in hand. A culled splat keeps the largest
// possible key and a zero radius, so it sorts behind everything and collapses
// to a quad the rasteriser discards.
//
// Every rejection here but one is exact: the splat is dropped only when it
// cannot colour a pixel. The exception is the depth-validity fade below, which
// weights a splat by how much of the Jacobian's linearisation still holds
// across its own depth extent and so does change the image.
#include "Splat.hlsli"
#include "SplatSort.hlsli"

ConstantBuffer<FrameConstants> g_frame : register(b0);

StructuredBuffer<Gaussian> g_gaussians : register(t0);

float3 shBand(Gaussian g, uint band)
{
    return float3(g.sh[band * 3], g.sh[band * 3 + 1], g.sh[band * 3 + 2]);
}

// The 3DGS basis, evaluated in the frame the splats were authored in: the
// degree-0 band is an offset from mid grey, the rest are view dependent.
// Negative radiance is clamped away rather than wrapped.
float3 evaluateSh(Gaussian g, float3 direction)
{
    const float x = direction.x, y = direction.y, z = direction.z;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, yz = y * z, xz = x * z;

    float3 colour = 0.28209479177387814f * shBand(g, 0);

    colour += 0.4886025119029199f * (-y * shBand(g, 1) + z * shBand(g, 2) - x * shBand(g, 3));

    colour +=  1.0925484305920792f * xy * shBand(g, 4);
    colour += -1.0925484305920792f * yz * shBand(g, 5);
    colour +=  0.31539156525252005f * (2.0f * zz - xx - yy) * shBand(g, 6);
    colour += -1.0925484305920792f * xz * shBand(g, 7);
    colour +=  0.5462742152960396f * (xx - yy) * shBand(g, 8);

    colour += -0.5900435899266435f * y * (3.0f * xx - yy) * shBand(g, 9);
    colour +=  2.890611442640554f * xy * z * shBand(g, 10);
    colour += -0.4570457994644658f * y * (4.0f * zz - xx - yy) * shBand(g, 11);
    colour +=  0.3731763325901154f * z * (2.0f * zz - 3.0f * xx - 3.0f * yy) * shBand(g, 12);
    colour += -0.4570457994644658f * x * (4.0f * zz - xx - yy) * shBand(g, 13);
    colour +=  1.445305721320277f * z * (xx - yy) * shBand(g, 14);
    colour += -0.5900435899266435f * x * (xx - 3.0f * yy) * shBand(g, 15);

    return max(colour + 0.5f, 0.0f);
}

float3x3 quaternionToMatrix(float4 q)
{
    const float w = q.w, x = q.x, y = q.y, z = q.z;
    return float3x3(
        1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z),        2.0f * (x * z + w * y),
        2.0f * (x * y + w * z),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x),
        2.0f * (x * z - w * y),        2.0f * (y * z + w * x),        1.0f - 2.0f * (x * x + y * y));
}

[numthreads(R4D_SORT_GROUP_SIZE, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x;
    if (index >= g_sort.count)
    {
        return;
    }

    // Padding past the real splats, so that every later kernel can run without
    // a bounds check.
    if (index >= g_sort.splatCount)
    {
        g_keysA[index] = 0xFFFFFFFFu;
        g_valuesA[index] = 0u;
        return;
    }

    g_keysA[index] = 0xFFFFFFFFu;
    g_valuesA[index] = index;

    // A culled splat still occupies an instance, so it is collapsed to a
    // zero-radius quad rather than left holding the previous frame's values.
    SplatProjection culled = (SplatProjection)0;
    g_projections[index] = culled;

    const Gaussian g = g_gaussians[index];

    // The rasteriser discards any sample under 1/255, and a splat's alpha is
    // at most its own opacity, so one stored below that floor cannot colour a
    // pixel wherever it lands. Testing it here rather than after the walk is
    // what stops it being projected, sorted, binned and read back out again.
    if (g.opacity < 1.0f / 255.0f)
    {
        return;
    }

    const float4 viewPosition = mul(g_frame.view, float4(g.position, 1.0f));
    if (viewPosition.z < g_frame.splatNearPlane)
    {
        return;   // behind the camera
    }

    // Sigma = R S S^T R^T, built in world space.
    const float3x3 rotation = quaternionToMatrix(g.rotation);
    const float3x3 scale = float3x3(g.scale.x, 0.0f, 0.0f,
                                    0.0f, g.scale.y, 0.0f,
                                    0.0f, 0.0f, g.scale.z);
    const float3x3 m = mul(rotation, scale);
    const float3x3 sigma = mul(m, transpose(m));

    const float focalX = g_frame.splatFocal.x;
    const float focalY = g_frame.splatFocal.y;
    const float invZ = 1.0f / viewPosition.z;
    const float invZ2 = invZ * invZ;

    // The Jacobian is a local linearisation, so it is only valid near the axis.
    // Clamping the off-axis ratio to 1.3 times the half-frustum, as the
    // reference implementation does, keeps a splat off to the side from
    // acquiring a covariance that smears it across the whole frame.
    const float limitX = 1.3f * 0.5f * g_frame.splatViewport.x / focalX;
    const float limitY = 1.3f * 0.5f * g_frame.splatViewport.y / focalY;
    const float clampedX = clamp(viewPosition.x * invZ, -limitX, limitX) * viewPosition.z;
    const float clampedY = clamp(viewPosition.y * invZ, -limitY, limitY) * viewPosition.z;

    // Screen y points down, so the second row carries the opposite sign to the
    // first. Getting that wrong mirrors every anisotropic splat.
    const float3x3 jacobian = float3x3(
        focalX * invZ,  0.0f,          -focalX * clampedX * invZ2,
        0.0f,          -focalY * invZ,   focalY * clampedY * invZ2,
        0.0f,           0.0f,            0.0f);

    const float3x3 viewRotation = (float3x3)g_frame.view;

    // The clamp above keeps the Jacobian honest off the axis; this keeps it
    // honest in depth. Linearising 1/z at the centre drops the terms quadratic
    // in the depth offset inside the splat, and because a Gaussian is
    // symmetric in depth the first-order over- and under-magnification cancel,
    // so the leading error left in the projected covariance goes as
    // (sigma_z / z)^2. Written in the three-sigma extent the renderer already
    // uses for the radius and the quad, that is this ratio squared, and at 1
    // the near end of that extent has reached the eye plane: the camera is
    // inside the splat and the projection has stopped meaning anything.
    //
    // Weighting the splat by how much of the linearisation survives therefore
    // leaves an ordinary splat alone -- a tenth of its depth away costs it one
    // per cent -- and fades out the ones the camera has walked into. Both ends
    // come from that convention rather than from a threshold picked to suit a
    // picture, and the fade is smooth in the camera's position, so nothing
    // pops as it moves.
    const float3 viewAxis = float3(viewRotation._31, viewRotation._32, viewRotation._33);
    const float depthExtent =
        3.0f * sqrt(max(dot(viewAxis, mul(sigma, viewAxis)), 0.0f)) * invZ;
    const float validity = saturate(1.0f - depthExtent * depthExtent);

    // Faded under the same alpha floor, the splat can no longer colour a pixel
    // either, so the early-out above applies again to what the fade leaves.
    const float opacity = g.opacity * validity;
    if (opacity < 1.0f / 255.0f)
    {
        return;
    }

    const float3x3 t = mul(jacobian, viewRotation);
    const float3x3 covariance = mul(mul(t, sigma), transpose(t));

    // A small dilation keeps very thin splats from falling between pixels.
    const float a = covariance._11 + 0.3f;
    const float b = covariance._12;
    const float c = covariance._22 + 0.3f;

    const float determinant = a * c - b * b;
    if (determinant <= 0.0f)
    {
        return;
    }

    const float4 clip = mul(g_frame.viewProjection, float4(g.position, 1.0f));
    if (clip.w <= 0.0f)
    {
        return;
    }

    const float invDeterminant = 1.0f / determinant;
    const float2 ndc = clip.xy / clip.w;
    const float2 screen = (ndc * float2(0.5f, -0.5f) + 0.5f) * g_frame.splatViewport;

    // Radius from the larger eigenvalue, at three standard deviations.
    const float mid = 0.5f * (a + c);
    const float spread = sqrt(max(mid * mid - determinant, 0.0001f));
    const float radius = 3.0f * sqrt(max(mid + spread, 0.0001f));

    // The quad this splat would emit, against the viewport. Outside it the
    // splat cannot reach a pixel, so dropping it changes nothing.
    if (screen.x + radius < 0.0f || screen.x - radius > g_frame.splatViewport.x ||
        screen.y + radius < 0.0f || screen.y - radius > g_frame.splatViewport.y)
    {
        return;
    }

    SplatProjection projection;
    projection.screen = screen;
    projection.radius = radius;
    projection.depth = clip.z / clip.w;
    projection.conic = float3(c * invDeterminant, -b * invDeterminant, a * invDeterminant);
    projection.opacity = opacity;
    projection.colour = evaluateSh(g, normalize(g.position - g_frame.cameraPosition));
    projection.padding = 0.0f;
    g_projections[index] = projection;

    // The same view-space z the projection used, so the order and the geometry
    // cannot disagree about which splat is in front.
    g_keysA[index] = splatDepthKey(viewPosition.z);
}
