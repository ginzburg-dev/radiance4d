# Changelog

All notable changes to Radiance4D.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

### Changed

### Removed

## [0.1.0] - 2026-10-16

### Added

- Win32 platform layer, logging, timing and error handling
- Direct3D 12 backend: explicit adapter selection, retail Agility SDK 1.619.6, Enhanced
  Barriers, flip-model swapchain, debug layer, DRED and validation counters
- Vulkan 1.3 backend: dynamic rendering, synchronization2, validation layers
- One HLSL source compiled to both DXIL and SPIR-V by a vendored DXC 1.9.2609
- Physically based shading with analytic lights, image-based lighting from a procedural sky,
  shadow mapping with PCF, and exposure control
- glTF 2.0 loading through cgltf, with base colour, metallic-roughness, normal and emissive maps
- Dear ImGui statistics overlay on both backends, drawable into a headless capture
- GPU timestamp profiling, per frame and per pass, with a reproducible `--benchmark` mode
- Headless rendering and frame capture
- 3D Gaussian Splatting: covariance projection to a screen-space conic, a compute cull pass, a
  GPU radix depth sort, tile binning and a tile rasteriser compositing front to back with
  per-pixel early termination
- Degree-3 spherical-harmonic colour evaluated per splat
- 3DGS `.ply` dataset loading, including the mirror out of COLMAP's frame
- Hybrid rendering: splats and meshes share one depth buffer
- Robust camera framing that ignores a reconstruction's far outliers
- `tools/reconstruct`: a dependency-free Python package that reads a COLMAP sparse model and
  exports it as a renderable `.ply`
- Test suite on GoogleTest and CTest: unit tests, WARP image regression, cross-API parity
- GitHub Actions for build, test and formatting, and a release workflow driven by tags

[Unreleased]: https://github.com/ginzburg-dev/radiance4d/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ginzburg-dev/radiance4d/releases/tag/v0.1.0
