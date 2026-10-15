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
the opacity sigmoid, the channel-major SH transposed into the GPU layout, and the mirror out of
COLMAP's frame. Published scenes are hundreds of megabytes to several gigabytes and cannot live in
the repository, so loading one stays a manual step, written down in the README.

A published scene has now been through it: INRIA's pre-trained `train`, 1,026,508 splats at SH
degree 3, 234.9 MiB of GPU attributes, zero validation errors on both backends, and the two
backends agreeing to a max channel delta of 1 (RMSE 0.0089) at 1280x720. Parse and convert takes
0.41 s and the upload 0.08 s in RelWithDebInfo.

That run settled the coordinate question. COLMAP's frame is right-handed with +y down and the
engine is left-handed with +y up, so the loader now mirrors y — positions, the rotation quaternion
as `(w, -x, y, -z)`, and the six SH bands odd in y. The direction was taken from the dataset rather
than assumed: averaging the image-down axis of the model's own 301 cameras lands 4.7° from +y. The
result is a rigid flip and nothing more — the corrected capture matches the vertical flip of the
un-mirrored one to RMSE 0.0111, where mirroring positions alone gives RMSE 5.2367.

Carrying degree 3 costs 48 of the 60 floats per splat: the GPU struct is 240 bytes, so a million
splats is 240 MB of attributes. Splitting the SH into its own buffer, so that culling and binning
only touch geometry, is the obvious follow-up once those passes exist.

The scene also put a number on why the remaining work is the remaining work. At 1920x1080 the frame
costs 93.2 ms of CPU against 23.1 ms of GPU: the full depth sort of 1.03M splats, single-threaded,
every frame. The GPU sort is no longer a tidiness argument.

One thing the run exposed that was not on the list above has since been fixed: the viewer framed the
camera on the cloud's bounding sphere, which a reconstruction's far-flung low-opacity splats stretch
far past the subject. `GaussianCloud` now carries a framing sphere beside its bounds — the per-axis
median position, and the distance half the splats are within — and the viewer points the camera at
that. On `train` it is radius 2.4 against the bounds' 145, which is a camera 5.5 units back instead
of 326. The dataset's 301 capture cameras stand a median 3.42 units from the centre, so that is
inside the volume the scene was photographed from. The bounds themselves are untouched.

Two things stood between `--splat-ply` and a screenshot worth keeping, both outside framing, and
both are now done:

- `Splat.vs.hlsl` did not clamp the Jacobian to the frustum. The Jacobian is a local linearisation
  and the reference implementation clamps the off-axis ratio to 1.3 times the half-frustum before
  building it; without that, a splat off to the side of a camera standing inside the reconstruction
  gets a covariance that covers the frame, and the capture is a near-white wash. With the clamp the
  same camera resolves into two locomotives, their lettering, the ballast and the hillside: the two
  captures differ over 99.98% of the frame. The procedural cloud barely notices — 33 of 230,400
  pixels move by one level at 640x360 — and the stored WARP references pass untouched.
- The orbit camera always started at yaw 0 and pitch 0, which for `train` points into foliage, and
  the viewer had no way to ask for a direction. `--orbit-yaw` and `--orbit-pitch` now set the
  starting angles, defaulting to 0. No direction is baked into the code for a particular scene;
  the angle that suits `train` is written down in the README beside the capture command.

`docs/images/train-dataset.png` is that capture: 1280x720, Debug, zero validation errors on both
backends, D3D12 against Vulkan at max channel delta 2 / RMSE 0.0447 over 0.49% of pixels, and
byte-identical between two runs of the same command.

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
