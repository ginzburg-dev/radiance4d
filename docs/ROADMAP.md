# Roadmap

Each milestone is independently verifiable. Current position: M3 complete and M4 under way — the
splat pass, `.ply` datasets and spherical-harmonic colour are in, the performance work is not.

## M0 — Environment and architecture

Toolchain inventory, dependency selection, repository and build system, architecture.

Findings that shaped the design:

- `D3D12SDKLayers.dll` is already in System32, so the debug layer needs no extra Windows feature.
- The Windows SDK copy of DXC has its SPIR-V backend stubbed out and cannot target Vulkan.
- Hybrid graphics laptop (RTX 3080 Laptop plus Intel UHD), so adapter selection must be explicit.
- Developer Mode is off, so only retail Agility SDK releases are usable.
- `core.autocrlf=true` globally, so `.gitattributes` is required to keep binary references intact.

## M1 — DirectX 12 foundation

### M1-A

Window, device and adapter selection, swapchain, frame synchronisation, resize, build-time HLSL
compilation, geometry, debug-layer validation.

Verified: warning-free build at `/W4 /WX`, correct adapter selected over the iGPU and WARP, feature
level 12_2, shader model 6_9, Enhanced Barriers supported, Agility redistributable confirmed loaded
at runtime, 600 frames with GPU-based validation and 12 swapchain recreations with zero debug-layer
errors and no live objects.

### M1-B

GPU timestamp profiling, headless offscreen rendering and capture, deterministic animation, WARP
image-regression tests, GitHub Actions CI.

Verified: 8/8 tests in Debug and RelWithDebInfo, reference image matches under optimisation,
byte-identical captures with a fixed time step and differing captures without one.

Not implemented in M1: depth buffer, backface culling, custom allocator, per-pass GPU timings.

## M2 — Interactive PBR viewer

Orbit and free-fly cameras, glTF loading through cgltf, Cook-Torrance GGX shading with three
analytic lights and a gradient environment term, base colour, metallic-roughness and normal maps
with CPU-generated mips, depth buffering, backface culling, and an ImGui statistics overlay.

Not implemented: image-based lighting, shadows, transparency sorting, texture compression.

## M3 — Vulkan backend

Native Vulkan 1.3 backend using dynamic rendering and synchronization2, selected at runtime with
`--backend vulkan`. Shaders reach SPIR-V through the same vendored DXC, so one compiler serves both
APIs.

Measured on an RTX 3080 Laptop GPU, the two backends agree to **max channel delta 1, RMSE 0.0018,
one differing pixel in 230400**. `BackendParity_test` asserts this automatically with a wider
allowance (delta 4, RMSE 0.5, 0.1% of pixels) so a driver revision does not fail the build.

The test is labelled `gpu` and excluded in CI: unlike D3D12 with WARP, there is no software Vulkan
device on the runner. CI builds D3D12 only, since the runner has no Vulkan SDK.

No shared cross-API abstraction was extracted. With both backends working, the only duplication that
turned out to be real is frame-constant assembly and the model/stats bookkeeping; the resource,
descriptor and synchronisation models differ enough that a common layer would have obscured more
than it shared. `Renderer` remains the single seam.

## Showcase polish

Done: image-based lighting, shadow mapping, a licensed glTF scene, the ImGui overlay on both
backends, exposure control, a reproducible benchmark mode and README screenshots.

Outstanding: GPU profiling with a real tool (PIX, Nsight Graphics or RenderDoc; none installed), a
scene heavy enough for the numbers to mean something, and a demo video.

## M4 — Static Gaussian splatting

Done: GPU attribute storage, 3D covariance projection to a screen-space conic, alpha compositing
back to front, a CPU depth sort, a procedural cloud for development and tests, a shared depth
buffer with the mesh pass, both backends drawing it from the same shaders, `.ply` dataset loading,
and degree-3 spherical-harmonic colour evaluated per splat from the view direction.

Remaining: visibility culling, tile-based binning, and a GPU sort. Those three are deliberately
last: they cannot be honestly measured against 20k procedural splats, and need the million-splat
workload a real dataset provides.

The `.ply` loader is covered by synthetic files the test suite writes itself, which assert the
conversions numerically — log-scales exponentiated, the quaternion normalised and read as `wxyz`,
the opacity sigmoid, and the channel-major SH transposed into the GPU layout. Published scenes are
hundreds of megabytes to several gigabytes and cannot live in the repository, so loading one is a
manual step, written down in the README. It has not been done yet.

Carrying degree 3 costs 48 of the 60 floats per splat: the GPU struct is 240 bytes, so a million
splats is 240 MB of attributes. Splitting the SH into its own buffer, so that culling and binning
only touch geometry, is the obvious follow-up once those passes exist.

No coordinate-system conversion is applied on load. Datasets authored in COLMAP's frame may appear
mirrored relative to their published renders; that is the first thing to check against a real scene.

## M5 — Reconstruction pipeline

COLMAP for pose estimation, PyTorch and gsplat for optimisation, a Python CLI for dataset processing
and training, export into the runtime. Requires COLMAP and a dedicated Python environment.

## M6 — Hybrid rendering

Mesh geometry and Gaussian representations in one scene, with depth-aware compositing.

## M7 — Temporal reconstruction

Motion vectors, reprojection, accumulation, history validation and rejection, disocclusion handling,
flicker and ghosting diagnostics, spatiotemporal denoising, temporal anti-aliasing. Intermediate
buffers stay accessible for research.

## M8 — Dynamic 4D reconstruction

Timestamps and camera trajectories, temporal deformation, playback, interpolation, temporal
consistency evaluation.

## M9 — Public release

Reproducible builds, documentation, diagrams, screenshots, videos, benchmarks on documented
hardware, articles, downloadable application.

## Showcase scenes

- Material Lab — PBR materials, metallic reflections, glass, lighting.
- Reconstructed Environment — photogrammetry or Gaussian reconstruction with free camera.
- Temporal Stress Test — moving camera and objects, dynamic lighting, comparison of temporal methods.

## Rules

- Benchmarks name hardware, driver, resolution, API, configuration and settings. The GPU here is an
  RTX 3080 Laptop GPU, not an RTX 3080.
- Unimplemented features stay marked as such.
- No claim that code compiles or runs without having built and run it.
- Where visual correctness cannot be checked automatically, manual steps are written down.
