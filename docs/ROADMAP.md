# Roadmap

Each milestone is independently verifiable. Current position: M2 complete, M3 next.

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

Equivalent rendering through Vulkan, validated against D3D12. The cross-API abstraction is derived
here, from two working backends. Shaders gain a SPIR-V target through the same DXC. Automated image
comparison between backends with stated tolerances. Requires the Vulkan SDK.

## M4 — Static Gaussian splatting

Dataset loading, GPU attribute storage, projection, screen-space covariance, visibility culling,
tile-based binning, GPU sorting, spherical-harmonic colour, alpha compositing, interactive camera.

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
