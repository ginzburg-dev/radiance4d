# Radiance4D

[![Windows build and test](https://github.com/ginzburg-dev/radiance4d/actions/workflows/windows_build_and_test.yaml/badge.svg?branch=main)](https://github.com/ginzburg-dev/radiance4d/actions/workflows/windows_build_and_test.yaml)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

A real-time renderer that draws triangle meshes and 3D Gaussian splats in the same frame, on
Direct3D 12 and Vulkan, from one set of HLSL sources.

Published Gaussian Splatting scenes render at a million splats in under ten milliseconds at 1080p,
through a compute pipeline that culls, depth-sorts, bins into tiles and composites front to back.

![INRIA train, 1.03M splats, with the per-pass overlay](docs/images/overlay.png)

---

## Features

- Modern C++20, CMake, no engine or rendering framework underneath
- Native Direct3D 12 and Vulkan 1.3 backends behind one narrow interface
- One HLSL source compiled to both DXIL and SPIR-V by a vendored DXC
- D3D12 Enhanced Barriers, the retail Agility SDK, DRED and validation counters
- Vulkan dynamic rendering and synchronization2
- 3D Gaussian Splatting end to end: covariance projection, compute culling, a GPU radix depth
  sort, tile binning and a tile rasteriser with per-pixel early termination
- 3DGS `.ply` datasets with degree-3 spherical harmonics, verified on published scenes
- Hybrid rendering: splats and meshes share one depth buffer
- Physically based shading with image-based lighting, shadow mapping and exposure control
- glTF 2.0 loading, including emissive, normal and metallic-roughness maps
- Per-pass GPU timestamps, in the overlay and in a reproducible benchmark mode
- Headless rendering, frame capture and image-regression tests on WARP
- Cross-API parity tests holding the two backends to the same image
- Python tooling that turns a COLMAP sparse model into a renderable scene

![Damaged Helmet](docs/images/damaged-helmet.png)

![Material grid](docs/images/material-grid.png)

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

The same scene cost 96 ms when the depth sort still ran on the CPU. Moving the sort to a compute
radix sort, projecting each splat once instead of once per quad corner, and replacing the quad
rasteriser with a tiled one that stops when a pixel saturates took it to under ten.

The two backends agree on the image to a maximum channel difference of 1 out of 255, which a test
enforces on every build.

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
