# Architecture

State as of milestone M3. See [ROADMAP.md](ROADMAP.md).

## Modules

```
app/viewer          radiance4d_viewer   no d3d12.h
include/radiance4d/graphics   r4d_graphics public surface, API-neutral
src/graphics/d3d12            all d3d12 / dxgi / Agility headers
src/graphics/vulkan           all vulkan headers
include/radiance4d/core       r4d_core    platform, no graphics API
```

| Target | Contents |
|---|---|
| `r4d_core` | Win32 window and input, logging, timing, filesystem, image I/O, GLM math, `Result`/`Status` |
| `r4d_graphics` | API-neutral public headers; D3D12 backend private to `src/graphics/d3d12/` |
| `radiance4d_viewer` | Application, plus the Agility SDK exports |

`r4d::Renderer` is the only polymorphic boundary. There are no cross-API `IBuffer`, `ITexture` or
`ICommandList` interfaces. Switching backend recreates the renderer.

With both backends working, that decision held: the resource, descriptor and synchronisation models
differ enough that a common layer would have obscured more than it shared. D3D12 binds a descriptor
table per material and pushes material data as root constants; Vulkan binds one descriptor set per
material with two dynamic uniform offsets. Only frame-constant assembly and the model and stats
bookkeeping are genuinely duplicated, and that is a smaller cost than the wrong abstraction.

`tests/IncludeHygiene_test.cpp` fails compilation if any public header pulls in `d3d12.h`, a `dxgi`
header or `windows.h`. `Window` exposes its HWND as `void*`.

## Frame synchronisation

| Index | Source | Selects |
|---|---|---|
| `backBufferIndex` | `GetCurrentBackBufferIndex()` | render target view only |
| `frameIndex` | `frameCounter % kFramesInFlight` | command allocator, fence slot, constant buffer |

Under the flip model the back-buffer index is not a round-robin counter. `kBackBufferCount` (3) and
`kFramesInFlight` (2) are unequal so conflating the two breaks immediately.

Per frame: wait on the frame-latency waitable object, wait for the fence to reach
`m_frames[frameIndex].fenceValue`, read the back-buffer index, write the frame constant buffer, reset
allocator and command list, record, execute, present, signal.

One command list is reused with per-frame allocators: a list may be reset immediately after
submission, an allocator only after its work completes.

Resize drains all in-flight frames before releasing back buffers. A zero client size suspends
rendering; `ResizeBuffers` rejects 0x0.

## Barriers

Enhanced Barriers throughout. They map closely onto Vulkan `VkImageMemoryBarrier2`.
`D3D12_FEATURE_D3D12_OPTIONS12::EnhancedBarriersSupported` is queried at startup; renderer creation
fails if it is absent rather than falling back to legacy resource states.

Sync scope and access type are validated against each other. `D3D12_BARRIER_ACCESS_INDEX_BUFFER`
pairs with `D3D12_BARRIER_SYNC_INDEX_INPUT`, not `SYNC_VERTEX_SHADING`.

## Headless rendering and capture

A null window handle means no swapchain: one offscreen target, nothing presented.

The target is created with `CreateCommittedResource3` and an explicit
`D3D12_BARRIER_LAYOUT_RENDER_TARGET`. A texture created in a legacy resource state only interops with
Enhanced Barriers from `COMMON`. This is why the device requirement is `ID3D12Device10`.

It stays in the render-target layout between frames; `captureFrame` does its own `COPY_SOURCE` round
trip. Readback strips the copy footprint padding, so captures are tightly packed RGBA8.

On the windowed path, capture uses the last rendered back buffer: after `Present` the current index
is the next buffer to draw into.

The render target carries an optimised clear value matching `ClearRenderTargetView`; without it the
driver cannot use its fast clear path.

`RendererDesc::fixedTimeStep` advances animation by a fixed amount per frame. Captures are not
reproducible on the wall clock.

## GPU profiling

Timestamps are written per frame slot and resolved into a readback buffer at end of frame. The result
is read after the fence wait the renderer already performs for allocator reuse, so profiling adds no
synchronisation. Reported GPU time lags the CPU by `kFramesInFlight` frames.

`GpuStats::gpuTimingsValid` is false until the first resolve; the GPU fields are unset, not zero. If
the queue reports no timestamp frequency, profiling is disabled.

## Vulkan backend

Vulkan 1.3 with `dynamicRendering` and `synchronization2`, both required at device creation; a device
that does not report them fails rather than falling back, matching how D3D12 treats Enhanced
Barriers.

Conventions that had to line up with D3D12:

| Concern | Handling |
|---|---|
| Clip space Y | Shaders are compiled with `-fvk-invert-y` (vertex stages only; the flag is illegal elsewhere) |
| Winding | `VK_FRONT_FACE_CLOCKWISE`, matching `FrontCounterClockwise = FALSE` |
| Depth range | `[0,1]` on both, already set by `GLM_FORCE_DEPTH_ZERO_TO_ONE` |
| Register bindings | HLSL register classes all land in set 0 and collide, so `-fvk-b/t/s/u-shift` separates them into 0, 100, 200 and 300 |
| Material data | Root constants on D3D12, a dynamic uniform buffer offset on Vulkan, so the shader is unchanged |

Physical device selection is explicit for the same reason as the DXGI path: on this machine the Intel
integrated GPU enumerates first, and an empty selector takes the first discrete device.

Validation messages go through `VK_EXT_debug_utils` into the same counting sink as the D3D12 debug
layer, so `validationErrorCount()` means the same thing on both backends.

## Validation

| Mechanism | Purpose |
|---|---|
| Debug layer | API validation, enabled before device creation |
| GPU-based validation | `--gpu-validation`; also synchronized command queue validation |
| DRED | Breadcrumbs and page faults, dumped on device removal or fence timeout |
| `ID3D12InfoQueue1` | Messages routed into the logger and counted by severity |
| `ReportLiveObjects` | After device teardown, drained from the DXGI info queue into the log |

`Renderer::validationErrorCount()` drives the viewer exit code. Counting happens before the runtime
log-level filter. D3D12 resources are released explicitly before the live-object report, since member
`ComPtr`s outlive the destructor body.

## Image regression

Structural assertions cover what a reference image cannot: corners are the clear colour, the centre
is not, alpha is opaque, the scene differs between frames, odd sizes round-trip. The reference
comparison reports worst channel deviation and RMSE separately.

WARP is pinned to a redistributable so CI and local runs use the same rasteriser. Measured on this
scene, the RTX 3080 Laptop GPU and WARP agree to within 1 of 255 per channel (RMSE 0.065, 1.65% of
pixels differing by one).

Comparison helpers live in `tests/`.

## Shaders

HLSL compiles to DXIL at build time only; the runtime loads `.dxil` blobs. No runtime compiler, no
hot-reload. A shader that fails to compile fails the build.

The standalone DXC release is used because the Windows SDK copy has its SPIR-V backend stubbed out.

Shaders are built with `-Zpc`, matching the GLM layout, so `glm::mat4` uploads without a transpose.
Transform vectors as `mul(M, v)`.

## Error handling

`Result<T>` and `Status` stand in for `std::expected`, which is C++23. Both are `[[nodiscard]]`.
`R4D_TRY` propagates; `R4D_HR` wraps HRESULT failures with `FormatMessage` text. No exceptions for
control flow.

## Naming

Namespace `r4d`, prefix `R4D_` for options and macros, `r4d_` for targets. PascalCase types,
camelCase functions, `m_` members, `k` constants. Headers `.hpp` under
`include/radiance4d/<module>/`, `src/` mirroring. Tests `tests/<Name>_test.cpp`.

`DeviceCapabilities` cannot be used as a type name: it is a `wingdi.h` macro that expands to
`DeviceCapabilitiesW` under `UNICODE`. The type is `GpuCapabilities`.

## Not implemented

- Depth buffer and backface culling (M2); the PSO uses `CULL_MODE_NONE`.
- Custom GPU allocator. M1 uses committed resources and one-shot uploads.
- Per-pass GPU timings; there is one timestamp pair around the frame.
- Benchmarks. The frame times the viewer prints are a timing aid, not measurements.
