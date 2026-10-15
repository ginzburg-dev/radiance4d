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
| 3DGS `.ply` dataset loading | working (verified on the INRIA `train` scene, 1.03M splats) |
| Spherical-harmonic colour (degree 3) | working (both backends) |
| Splat visibility culling, tiled binning, GPU sorting | planned (M4) |
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
evaluates the conic and emits premultiplied alpha, and the splats are composited back to front from
a CPU depth sort.

Without `--scene` the splats replace the mesh scene; with one they share its depth buffer, so
geometry occludes the splats behind it. Both backends draw the pass from the same HLSL, and the
parity test holds them to the same image.

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

Current limits: one draw call per cloud with no visibility culling or tiled binning, a full CPU sort
every frame, and 240 bytes of attributes per splat (degree-3 SH is 192 of them), which is 240 MB for
a million-splat scene.

Framing a real scene is also still crude. The viewer points the camera at the cloud's bounding
sphere, and reconstruction leaves sparse low-opacity splats far outside the subject — in `train`,
x spans about 149 units while its interquartile range is about 4 — so the bounding radius is
dominated by outliers and the subject lands in a few percent of the frame. A robust framing that
ignores the tail is needed before `--splat-ply` gives a presentable image, and that is why there is
no dataset screenshot here yet.

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
| INRIA `train`, 1.03M splats | 1920x1080 | D3D12 | 93.189 ms | 23.055 ms |
| INRIA `train`, 1.03M splats | 1920x1080 | Vulkan | 93.359 ms | 23.271 ms |

**What these do and do not show.** The mesh scene never loads the GPU: 1.18 million triangles cost
0.41 ms, and quadrupling the pixel count to 4K only reaches 0.57 ms, so the frame is bounded by
fixed per-frame cost rather than by geometry or fill. Headless means no present, so swapchain cost
is excluded.

The one thing the numbers do support is where the backends differ. On the light scene Vulkan is
0.05 ms behind on CPU; by 4K the two are within 0.006 ms of each other. The gap is command
submission overhead, which stops mattering as soon as the frame has real work in it, and it is not a
statement about shading performance. Drawing a D3D12-versus-Vulkan conclusion from this would be
wrong.

The splat row is the opposite case, and it is the first scene here heavy enough to be worth
measuring. 93 ms of CPU against 23 ms of GPU is a frame spent sorting: every one of the 1.03M
splats is depth-sorted on the CPU, every frame, single-threaded, and the draw itself is the smaller
half. Both backends land within 0.2 ms of each other because they run the same sort — that number
says nothing about D3D12 against Vulkan and everything about where the remaining M4 work is. The
GPU figure is also not a shading measurement while the pass is one undivided draw with no culling
or binning.

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
