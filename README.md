# Radiance4D

[![Windows build and test](https://github.com/ginzburg-dev/radiance4d/actions/workflows/windows_build_and_test.yaml/badge.svg?branch=main)](https://github.com/ginzburg-dev/radiance4d/actions/workflows/windows_build_and_test.yaml)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

**A rendering engine for Gaussian splats and meshes.** Direct3D 12 and Vulkan, HLSL shaders.

1,000,000 splats at 1080p — 9.7 ms.

![INRIA train, 1.03M splats, with the per-pass overlay](docs/images/overlay.png)

---

## Why

A photogrammetry scan is a cloud of Gaussians. Everything else is triangles. Splat viewers cannot
draw models, engines treat scans as an import. This draws both.

- **Virtual production and previs** — put CG into a scanned location and walk it live
- **VFX layout** — a scanned environment behind a CG element instead of a flat plate
- **Digital twins** — a scanned site with CAD or markup on top
- **A base to build on** — the whole splatting pipeline, readable, in one repo

The reference splatting code is CUDA, so it needs an NVIDIA card. This one is plain HLSL and runs
on any Direct3D 12 or Vulkan GPU.

![Damaged Helmet](docs/images/damaged-helmet.png)

---

## Features

- C++20 and CMake, no engine underneath
- Direct3D 12 and Vulkan 1.3 backends, one HLSL source compiled for both
- Gaussian splatting: compute culling, GPU radix sort, tiled rasteriser
- 3DGS `.ply` datasets with degree-3 spherical harmonics
- glTF 2.0, PBR shading, image-based lighting, shadows
- Per-pass GPU timings in the overlay and in a benchmark mode
- Headless rendering, image-regression and cross-API tests
- COLMAP sparse models exported to renderable splats

---

## Performance

INRIA's pre-trained `train` scene, 1,026,508 splats at spherical-harmonic degree 3, at 1920x1080.
RTX 3080 Laptop GPU, driver 617.14, i7-11800H, RelWithDebInfo, vsync off, mean of 200 measured
frames after a warm-up.

| Pass | D3D12 | Vulkan |
|---|---|---|
| Cull | 0.91 ms | 0.74 ms |
| Depth sort | 0.81 ms | 0.74 ms |
| Binning | 1.38 ms | 1.27 ms |
| Tile sort | 2.74 ms | 4.00 ms |
| Rasterise | 3.82 ms | 3.59 ms |
| Composite | 0.04 ms | 0.04 ms |
| **Frame** | **9.70 ms** | **10.38 ms** |

The same scene cost 96 ms with the sort on the CPU. The GPU radix sort, the compute cull pass and
the tiled rasteriser took it to under ten.

Both backends produce the same image to within 1 of 255 per channel. A test checks it on every
build.

[docs/RENDERING.md](docs/RENDERING.md) has the pipeline in detail and what every change measured.

---

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

Load a scan or a model:

```
radiance4d_viewer.exe --splat-ply point_cloud.ply
radiance4d_viewer.exe --scene model.gltf --splats 2000
radiance4d_viewer.exe --backend vulkan
```

Mouse orbits, pans and zooms. WASDQE in free-fly. F1 vsync, F2 camera mode, Esc quits.
`--help` lists every flag.

Render without a window, and without a GPU:

```
radiance4d_viewer.exe --headless --frames 30 --out frame.png
radiance4d_viewer.exe --splat-ply point_cloud.ply --benchmark 200
```

## Documentation

- [docs/RENDERING.md](docs/RENDERING.md) — the splatting pipeline and its measurements
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module boundaries and the backend seam
- [docs/BUILDING.md](docs/BUILDING.md) — toolchain and dependencies
- [docs/ROADMAP.md](docs/ROADMAP.md) — milestones
- [tools/reconstruct/README.md](tools/reconstruct/README.md) — COLMAP to renderable splats
- [CHANGELOG.md](CHANGELOG.md)

## License

MIT, see [LICENSE](LICENSE).

Fetched at configure time under their own licenses: GLM (MIT), GoogleTest (BSD-3-Clause), DirectX
Shader Compiler (LLVM/NCSA + MIT), DirectX Agility SDK and WARP (Microsoft; `D3D12Core.dll`,
`d3d12SDKLayers.dll`, `d3dconfig.exe` and `d3d10warp.dll` are redistributable per the package terms),
stb (MIT / public domain), cgltf (MIT), Dear ImGui (MIT).
