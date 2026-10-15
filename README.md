# Radiance4D

Hybrid real-time rendering and spatiotemporal reconstruction engine featuring 3D/4D Gaussian
Splatting, photogrammetry, ray tracing, and temporal stability.

C++20, with native Direct3D 12 and Vulkan backends.

## Status

Milestone M3 complete, M4 under way. Features marked *planned* are not implemented.

| Area | Status |
|---|---|
| Win32 platform layer, logging, timing | working |
| D3D12 device, explicit adapter selection | working |
| DirectX Agility SDK (retail 1.619.6) | working |
| Enhanced Barriers | working |
| Swapchain, frame synchronisation, resize | working |
| Build-time HLSL to DXIL (DXC 1.9.2609) | working |
| Debug layer, DRED, validation counters | working |
| Indexed and non-indexed geometry | working |
| GPU timestamp profiling | working |
| Headless rendering and frame capture | working |
| WARP image-regression tests, CI | working |
| Depth buffering | working |
| Orbit and free-fly cameras | working |
| glTF loading (cgltf) | working |
| PBR shading, analytic lights | working |
| Base colour, metallic-roughness, normal maps | working |
| ImGui statistics overlay | working (both backends) |
| Image-based lighting (procedural sky) | working |
| Shadow mapping (key light, PCF) | working |
| Exposure control | working |
| Vulkan backend (1.3, dynamic rendering, synchronization2) | working |
| Cross-API image parity test | working |
| 3D Gaussian Splatting (projection, sorting, compositing) | working (both backends) |
| 3DGS `.ply` dataset loading | working (verified on the INRIA `train` and `truck` scenes, 1.03M and 2.54M splats) |
| Spherical-harmonic colour (degree 3) | working (both backends) |
| Splat GPU depth sorting | working (compute radix sort, both backends) |
| Splat visibility culling, tiled binning | planned (M4) |
| COLMAP / gsplat reconstruction | planned (M5) |
| Hybrid mesh and Gaussian rendering | working (shared depth buffer) |
| Temporal reprojection and denoising | planned (M7) |
| Dynamic 4D Gaussians | planned (M8) |

![Material grid](docs/images/material-grid.png)

## Gaussian splatting

`--splats <n>` renders a deterministic procedural cloud of 3D Gaussians, and `--splat-ply <file>`
loads a published 3DGS dataset instead. Each splat carries a position, a linear scale, a rotation
quaternion, an opacity and 16 spherical-harmonic coefficients per channel; the vertex shader builds
the 3D covariance, projects it with the perspective Jacobian into a screen-space conic, evaluates
the SH colour from the view direction, and expands a quad from `SV_VertexID`. The pixel shader
evaluates the conic and emits premultiplied alpha, and the splats are composited back to front in
the order a compute radix sort writes earlier in the same frame.

Without `--scene` the splats replace the mesh scene; with one they share its depth buffer, so
geometry occludes the splats behind it. Both backends draw the pass from the same HLSL, and the
parity test holds them to the same image.

### The depth sort

Alpha compositing is order-dependent, so every splat has to be ordered against every other one each
time the camera moves. That sort runs on the GPU: five compute kernels, four passes over an 8-bit
digit of a 32-bit key, compiled from one HLSL source to both DXIL and SPIR-V like everything else
here.

The key is the view-space depth under a monotonic bit transform rather than a quantised range, so
the ordering is exact, and the sort is stable, so equal depths keep their buffer order. On `train`
that is enough for the GPU order to reproduce the CPU sort it replaced **pixel for pixel**: 0 of
921,600 pixels differ at 1280x720, on both backends. Backend parity is unchanged by the switch, at
the same max channel delta 23 and RMSE 0.6806 the CPU sort produced on this view.

Per pass, each tile of 256 splats counts its digits into a histogram written digit-major; one group
per digit scans that digit's row; one group scans the 256 row totals; and each tile then scatters,
taking its destination as the digit's offset in the frame plus the tile's offset within the digit
plus the element's rank among the tile's elements sharing it. That rank comes from a 256-bit mask
per digit, one groupshared atomic per element. Scratch is per frame in flight, so the sort needs no
barrier against the previous frame's reads: the per-slot fence wait has already happened.

The order buffer moved from a CPU-visible upload heap to device-local memory as part of this, which
also takes the vertex shader's per-instance order fetch off the PCIe bus.

```
radiance4d_viewer.exe --splats 20000
radiance4d_viewer.exe --splat-ply path/to/point_cloud.ply
radiance4d_viewer.exe --splats 1200 --scene assets/DamagedHelmet/DamagedHelmet.glb
```

![Gaussian splats](docs/images/gaussian-splats.png)

### Datasets

The loader takes the binary little-endian `.ply` the 3DGS reference implementation writes. Header
properties are matched by name, since exporters disagree on field order, and the stored activations
are converted on load: scales are exponentiated, the `wxyz` quaternion is normalised, and the
opacity goes through a sigmoid. Degree 0, 1, 2 and 3 files all load; bands the file does not carry
stay zero. ASCII and big-endian files, list properties and multi-element files are refused with an
error rather than guessed at.

Published scenes are hundreds of megabytes to several gigabytes, so none is committed here and the
test suite writes its own synthetic files instead. Loading one is a manual step: download a trained
`point_cloud.ply` (for example from the INRIA 3DGS release) and pass it to `--splat-ply`.

**Coordinate frame.** COLMAP's world frame, which 3DGS trains in, is right-handed with +y down,
while the engine is left-handed with +y up, so the loader mirrors y. Positions, the rotation
quaternion and the spherical harmonics all have to move together. With `M = diag(1, -1, 1)` the
covariance `M (R S S^T R^T) M^T` keeps its scales and takes the rotation `M R M`, which is the
quaternion `(w, x, y, z)` becoming `(w, -x, y, -z)`; the six SH bands whose basis polynomial is odd
in y change sign, so the colour along a direction survives the flip. Without the mirror a published
scene loads upside down.

That frame was not guessed. The INRIA `train` model ships a `cameras.json`, and averaging the
image-down axis of its 301 cameras gives a world direction 4.7° off +y, with 297 of the 301 within
25° of that mean — the capture's own idea of "down" is the file's +y.

**What a published scene actually did.** INRIA 3DGS pre-trained `train` (Tanks and Temples),
`point_cloud/iteration_30000/point_cloud.ply`: 1,026,508 splats, 62 properties, SH degree 3,
243 MiB on disk and 234.9 MiB of GPU attributes. Headless at 1280x720, 8 frames, fixed step, RTX
3080 Laptop GPU:

- Zero validation errors on both backends, in Debug with the debug layer and validation layer on
  and in RelWithDebInfo.
- D3D12 against Vulkan: max channel delta 1, RMSE 0.0089, 172 of 921,600 pixels differ. Debug and
  RelWithDebInfo captures are byte-identical within a backend.
- Parse and convert 0.41 s, upload 0.08 s (RelWithDebInfo, warm file cache, i7-11800H); 2.0 s and
  0.25 s in Debug.
- The mirror is a rigid flip and nothing else: the corrected capture matches the vertical flip of
  the un-mirrored one to max delta 5 / RMSE 0.0111. Mirroring positions alone, leaving the rotation
  and SH in the file's frame, gives max delta 244 / RMSE 5.2367 against the same reference, which is
  what makes that comparison worth running.
- The colour is view-dependent, not flat. Rendering a degree-0 copy of the same file — identical
  geometry, `f_rest` dropped — differs by max channel delta 77, RMSE 0.9092 over 13,494 pixels.
  Across the splats themselves the colour moves a median 0.19 in 0..1 units over ten view
  directions, against a median degree-0 contribution of 0.28.

**A second scene, 2.5x the size.** INRIA 3DGS pre-trained `truck`, same release and the same
`point_cloud/iteration_30000/point_cloud.ply`: 2,541,226 splats, 62 properties, SH degree 3, 601 MiB
on disk, 581.6 MiB of GPU attributes and 96.9 MiB of sort scratch across both frames in flight.

- Zero validation errors on both backends, Debug with the debug layer and the validation layer on,
  and the D3D12 live-object report is empty.
- Parse and convert 1.17 s, upload 0.15 s (RelWithDebInfo, warm file cache), against 0.47 s and
  0.07 s for `train` on the same build — 2.48x and 2.32x the time for 2.48x the splats, so loading
  is linear in the splat count.
- Nothing degrades with the size. 678.5 MiB of splat buffers against 15.80 GiB of adapter memory,
  and a procedural 4M-splat cloud (915 MiB of attributes, 152 MiB of scratch) runs on both backends
  as well.
- The capture is a featureless white field, which is the Jacobian defect below and not something the
  larger scene introduced. Both the parity figure and the frame times have to be read with that in
  mind, and both are given below with and without a clamp applied as a probe.

Current limits: one draw call per cloud with no visibility culling or tiled binning, and 240 bytes
of attributes per splat (degree-3 SH is 192 of them), which is 240 MB for a million-splat scene --
of which the sort reads only the 12 bytes of position. The sort itself adds 39.2 MiB of scratch at
1.03M splats and 96.9 MiB at `truck`'s 2.54M, across both frames in flight. Both are linear in the
splat count, so the 6.13M-splat `bicycle` scene would want about 1.4 GiB of attributes and 233 MiB
of scratch.

A splat cloud is no longer framed on its bounding sphere. A reconstruction leaves sparse
low-opacity splats far outside the subject, so that sphere is dominated by outliers: in `train` it
has radius 145 about a centre 35 units from the scene, and the camera ended up 326 units back with
the subject a few percent of the frame. The cloud now also carries a framing sphere — the per-axis
median position, and the distance half the splats are within — which `train` puts at radius 2.4 and
the camera 5.5 back. The full bounds are unchanged and still carry culling, the near and far planes
and the merge with a mesh's bounds; mesh scenes still frame exactly as before.

The dataset's own capture poses say that is the right place to stand: the 301 cameras in `train`'s
`cameras.json` sit a median 3.42 and at most 7.35 units from the scene centre, and the median splat
position lands 0.07 units from the median of those camera positions.

That agreement does not generalise, and `truck` is where it breaks. Its 251 capture cameras stand a
median 4.12 and at most 6.40 units from the scene centre, but half its splats lie 12.5 units or
further out — it reconstructs a large surrounding background rather than a subject on a plinth —
so the framing sphere comes out at radius 12.6 and the camera 28 units back, four times beyond the
furthest camera that ever photographed it. That is still far better than the bounding sphere, which
has radius 111.6 and would put the camera 251 units back, but "the distance half the splats are
within" turns out to measure how much background a scene reconstructed rather than where it was shot
from. Checking the framing radius against the dataset's own `cameras.json` is the part worth
keeping; a heuristic that reads the capture poses when a dataset ships them would be better still.

There is still no dataset screenshot here, for two reasons that framing does not reach. The
projection's Jacobian is not clamped to the frustum the way the reference implementation clamps it,
so once the camera is close enough to be inside the reconstruction, off-axis splats acquire a
covariance that smears them across the whole frame and the capture is a white wash; that is a
projection defect, measured and written up, not a framing one. And the orbit camera always starts
at yaw 0, which for `train` looks into foliage. With both addressed the scene does resolve — two
locomotives, lettering, ballast and the hillside behind — but neither is fixed on `main` yet.

`truck` hits the same defect, and harder. At its framing camera every pixel of the 1280x720 capture
lands between 253 and 255 on all three channels: 100% of pixels at or above 250, standard deviation
under 1.1, no structure at all. Applying the clamp as a local probe on the same build renders the
scene instead — foliage, the red truck, the ground behind it — with values spread over 0..255 and
only 0.37% of pixels near white. The probe was measured and reverted, not committed here.

That is what the parity figure turns on. Measured on the washed-out image, D3D12 against Vulkan on
`truck` gives max channel delta 1, RMSE 0.0079, 74 of 921,600 pixels, which is two featureless
fields agreeing and says nothing. Measured with the clamp, the same comparison is max channel
delta 2, RMSE 0.0380, 3,360 pixels, and that is the honest parity statement for this scene.

## Measurements

`--benchmark <n>` renders headless with the debug layer off, discards 64 warm-up frames and reports
CPU and GPU frame time. GPU time comes from timestamp queries around the frame. `--grid <cols>x<rows>`
sizes the built-in scene.

RTX 3080 Laptop GPU (GA104, 16 GB), driver 617.14, i7-11800H, RelWithDebInfo, vsync off, mean of the
measured frames:

| Scene | Resolution | Backend | CPU | GPU |
|---|---|---|---|---|
| 7x5 grid, 36 draws, 108k tris | 1920x1080 | D3D12 | 0.164 ms | 0.142 ms |
| 7x5 grid, 36 draws, 108k tris | 1920x1080 | Vulkan | 0.214 ms | 0.148 ms |
| 24x16 grid, 385 draws, 1.18M tris | 1920x1080 | D3D12 | 0.433 ms | 0.412 ms |
| 24x16 grid, 385 draws, 1.18M tris | 1920x1080 | Vulkan | 0.408 ms | 0.379 ms |
| 24x16 grid, 385 draws, 1.18M tris | 3840x2160 | D3D12 | 0.599 ms | 0.574 ms |
| 24x16 grid, 385 draws, 1.18M tris | 3840x2160 | Vulkan | 0.593 ms | 0.568 ms |
| INRIA `train`, 1.03M splats, CPU sort | 1920x1080 | D3D12 | 96.074 ms | 35.285 ms |
| INRIA `train`, 1.03M splats, CPU sort | 1920x1080 | Vulkan | 99.731 ms | 32.387 ms |
| INRIA `train`, 1.03M splats, GPU sort | 1920x1080 | D3D12 | 22.475 ms | 22.455 ms |
| INRIA `train`, 1.03M splats, GPU sort | 1920x1080 | Vulkan | 18.707 ms | 18.677 ms |
| INRIA `truck`, 2.54M splats, GPU sort | 1920x1080 | D3D12 | 14.718 ms | 14.698 ms |
| INRIA `truck`, 2.54M splats, GPU sort | 1920x1080 | Vulkan | 14.045 ms | 14.018 ms |

**What these do and do not show.** The mesh scene never loads the GPU: 1.18 million triangles cost
0.41 ms, and quadrupling the pixel count to 4K only reaches 0.57 ms, so the frame is bounded by
fixed per-frame cost rather than by geometry or fill. Headless means no present, so swapchain cost
is excluded.

The one thing the numbers do support is where the backends differ. On the light scene Vulkan is
0.05 ms behind on CPU; by 4K the two are within 0.006 ms of each other. The gap is command
submission overhead, which stops mattering as soon as the frame has real work in it, and it is not a
statement about shading performance. Drawing a D3D12-versus-Vulkan conclusion from this would be
wrong.

The splat rows are the opposite case, and they are the first scene here heavy enough to be worth
measuring. The two CPU-sort rows are what the pass cost before the sort moved: 96 and 100 ms of CPU
against 35 and 32 ms of GPU, a frame spent sorting, with every one of the 1.03M splats depth-sorted
on one core every frame. Both backends landed within 4 ms of each other because they ran the same
sort; that said nothing about D3D12 against Vulkan.

Moving the sort to the GPU takes the CPU frame to 22.475 and 18.707 ms, and the CPU figure is now
within 0.03 ms of the GPU one on both backends — the CPU is no longer doing the work, it is waiting
for the GPU, and the frame is GPU-bound for the first time.

The GPU figure fell as well, from 35.285 to 22.455 ms on D3D12. That is not the sort being free: it
is mostly the order buffer leaving the upload heap, which stops the vertex shader fetching a
`uint` per instance across PCIe. A control at 20,000 splats, where neither build is CPU-bound and
the GPU is loaded the same either way, puts the two within 0.062 ms of each other (2.414 against
2.476 ms), so the difference at a million splats is work removed rather than a clock artefact.

The CPU and GPU figures stay locked together at 2.54M splats — 14.718 against 14.698 ms on D3D12,
14.045 against 14.018 on Vulkan — so nothing has crept back onto the CPU at 2.5x the work.

**2.5x the splats is cheaper, not dearer, and that is the result.** `truck` carries 2,541,226 splats
against `train`'s 1,026,508, and its frame costs 14.698 ms of GPU against `train`'s 22.050 ms on
D3D12. (`train` was re-run alongside it on the same build and reproduced the published row to within
0.5 ms; the `truck` rows are the mean of three 100-frame runs.) The frame is not bounded by the
splat count. It is bounded by rasterised area, and at their respective framing cameras `train`
covers far more screen per splat than `truck` does. The defect above is not what produces that
ordering either: with the clamp applied as a probe, the same comparison is `truck` 9.432 ms against
`train` 14.470 ms, so both get cheaper and `truck` stays the cheaper one.

Shrinking the render target to 160x90 removes essentially all of the fill and leaves the
view-independent work, which is the sort plus the per-splat projection. Mean GPU time over three
100-frame runs, clamp probe applied so both scenes project the same way:

| 160x90 | `train`, 1.03M | `truck`, 2.54M | ratio |
|---|---|---|---|
| D3D12 | 2.472 ms | 6.123 ms | 2.477 |
| Vulkan | 2.182 ms | 7.217 ms | 3.308 |

Against a splat-count ratio of 2.476, D3D12 is linear to within 0.1%. Vulkan is not: it is 12%
cheaper than D3D12 at a million splats and 18% dearer at 2.5 million. A procedural sweep, which
holds the data distribution fixed and varies only the count, puts the crossover between 2M and
2.54M splats and has the gap still widening at 4M:

| 160x90, GPU | 250k | 500k | 1M | 2M | 2.54M | 4M |
|---|---|---|---|---|---|---|
| D3D12 | 0.886 | 1.669 | 3.432 | 7.127 | 8.105 | 13.279 ms |
| Vulkan | 0.764 | 1.620 | 3.340 | 6.863 | 8.960 | 14.698 ms |

From 1M to 4M D3D12 goes up 3.87x for 4x the splats; Vulkan goes up 4.40x. Which of the five sort
kernels or the draw accounts for it is not something these numbers separate, because there is no
per-pass timing yet. That is what the missing profiler is for, and it is now the first thing worth
building.

What is left in the frame is still the draw. Every number here is one undivided draw call with no
visibility culling and no tiled binning, which is exactly the remaining M4 work — and the fact that
fill, not splat count, sets the frame is the argument for doing the binning.

A real profiler is still outstanding.

## Building

Windows 11, Visual Studio 2022 (v143), CMake 3.26+, a D3D12 GPU. Dependencies are fetched and
hash-pinned by CMake; nothing is installed system-wide.

```
cmake --preset windows-d3d12
cmake --build --preset windows-d3d12-debug
ctest --preset windows-d3d12-debug
```

See [docs/BUILDING.md](docs/BUILDING.md).

## Running

```
build/windows-d3d12/bin/Debug/radiance4d_viewer.exe
```

`--backend <d3d12|vulkan>`, `--scene <file.gltf>`, `--splats <n>`, `--splat-ply <file.ply>`,
`--adapter <index|warp|name>`, `--no-vsync`, `--gpu-validation`,
`--frames <n>`, `--resize-cycles <n>`, `--log-level <0..5>`. `--help` lists all flags.
Without `--scene` a built-in material grid is rendered. Esc quits, F1 toggles vsync, F2 switches
camera mode; left mouse orbits, right pans, wheel zooms, WASDQE moves in free-fly. Exit code is non-zero if the
debug layer reported an error.

Headless, no GPU required:

```
radiance4d_viewer.exe --headless --frames 30 --fixed-step 0.0166667 --out frame.png
```

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/ROADMAP.md](docs/ROADMAP.md)
- [docs/BUILDING.md](docs/BUILDING.md)

## License

MIT, see [LICENSE](LICENSE).

Fetched at configure time under their own licenses: GLM (MIT), GoogleTest (BSD-3-Clause), DirectX
Shader Compiler (LLVM/NCSA + MIT), DirectX Agility SDK and WARP (Microsoft; `D3D12Core.dll`,
`d3d12SDKLayers.dll`, `d3dconfig.exe` and `d3d10warp.dll` are redistributable per the package terms),
stb (MIT / public domain), cgltf (MIT), Dear ImGui (MIT).
