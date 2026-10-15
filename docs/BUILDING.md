# Building

## Requirements

| | Minimum | Verified on |
|---|---|---|
| OS | Windows 10 1909+ | Windows 11 build 26300 |
| Toolchain | Visual Studio 2022, v143 | VS 2022 17.14, MSVC 14.44.35207, cl 19.44.35215 |
| Windows SDK | 10.0.22621.0+ | 10.0.26100.0 (pinned via `CMAKE_SYSTEM_VERSION`) |
| CMake | 3.26+ | 3.28.3 |
| GPU | D3D12 with Enhanced Barriers; Vulkan 1.3 | RTX 3080 Laptop GPU, driver 617.14 |
| Vulkan SDK | 1.3+ for the validation layers | 1.4.363.0 |

Only Visual Studio and CMake need installing. The debug layer works without the Graphics Tools
feature; the Agility SDK supplies its own matching `d3d12SDKLayers.dll`.

## Dependencies

Fetched at configure time, hash-pinned where downloaded as archives. Nothing is installed
system-wide or vendored into git.

| Dependency | Version | Notes |
|---|---|---|
| GLM | 1.0.1 | Math |
| GoogleTest | 1.14.0 | Tests |
| DirectX Shader Compiler | v1.9.2609 | Standalone; the Windows SDK copy cannot emit SPIR-V |
| DirectX Agility SDK | 1.619.6 retail | Preview releases require Developer Mode |
| Microsoft.Direct3D.WARP | 1.0.21 | Pinned so CI and local use the same rasteriser |
| stb_image / stb_image_write | pinned commit | PNG read and write |

## Configure and build

```bash
cmake --preset windows-d3d12
cmake --build --preset windows-d3d12-debug
ctest --preset windows-d3d12-debug
```

The `windows-d3d12` preset uses the Visual Studio generator and needs no developer command prompt.
It is multi-config:

| Build preset | Configuration |
|---|---|
| `windows-d3d12-debug` | Debug |
| `windows-d3d12-relwithdebinfo` | RelWithDebInfo, use for measurement |
| `windows-d3d12-release` | Release |

`windows-d3d12-ninja` is available for faster iteration but must be run from a Visual Studio
developer command prompt.

### Options

| Option | Default | Meaning |
|---|---|---|
| `R4D_ENABLE_D3D12` | ON | D3D12 backend |
| `R4D_ENABLE_VULKAN` | ON | Needs the LunarG SDK; CI builds with it OFF |
| `R4D_ENABLE_AGILITY_SDK` | ON | Use the redistributable runtime |
| `R4D_BUILD_TESTS` | ON | Build and register tests |
| `R4D_WARNINGS_AS_ERRORS` | ON | `/WX` on first-party targets only |
| `R4D_MIN_LOG_LEVEL` | 0 | Compile-time minimum log level |

## Running

| Flag | Meaning |
|---|---|
| `--backend <d3d12\|vulkan>` | Graphics API, default d3d12 |
| `--adapter <sel>` | Empty picks the fastest hardware adapter; also `warp` (D3D12 only), an index, or part of a name |
| `--width <n>` `--height <n>` | Initial client size |
| `--vsync` / `--no-vsync` | Present interval |
| `--debug` / `--no-debug` | Debug layer, on by default |
| `--gpu-validation` | GPU-based validation; slow, never benchmark with it |
| `--frames <n>` | Exit after n frames |
| `--resize-cycles <n>` | Walk the window through a set of sizes |
| `--grid <cols>x<rows>` | Size of the built-in material grid |
| `--benchmark <n>` | Measure n frames after a warm-up; forces headless and no debug layer |
| `--headless` | No window, offscreen target |
| `--out <file.png>` | Write the last rendered frame. Vulkan supports this in `--headless` only; D3D12 can also capture a window. |
| `--fixed-step <seconds>` | Fixed animation step; required for a reproducible capture |
| `--log-level <0..5>` | 0 trace, 2 info, 4 error |

Esc quits, F1 toggles vsync. Exit code is non-zero if the debug layer reported an error.

## Verification

```bash
cmake --preset windows-d3d12
cmake --build --preset windows-d3d12-debug     # warning-free at /W4 /WX
ctest --preset windows-d3d12-debug             # 8/8

cd build/windows-d3d12/bin/Debug
./radiance4d_viewer.exe --frames 600 --no-vsync --gpu-validation --resize-cycles 3
echo $?                                        # 0
```

The run must report `Validation clean: 0 debug-layer errors` and
`live-object report produced no entries`.

Check the startup log names the right adapter:

```
Adapter: SELECTED NVIDIA GeForce RTX 3080 Laptop GPU ...
Device: feature level 12_2, shader model 6_9, enhanced barriers yes, ...
Device: runtime = Agility SDK redistributable ...
```

### Reference images

`tests/data/references/warp/` holds PNGs rendered on the pinned WARP. To regenerate after an
intentional change, delete the reference and run the test: it writes a `.actual.png` candidate and
fails. Inspect it before renaming it into place. On a mismatch the test writes `.actual.png` and
`.diff.png` beside the reference, and CI uploads both.

### Manual checks

Run the viewer with no arguments:

1. A 1600x900 window opens.
2. A rotating triangle is drawn over a darker quad backdrop.
3. Resizing, maximise, minimise and restore produce no corruption.
4. F1 toggles vsync.
5. The title bar shows a GPU time after the first few frames.
6. Esc exits with zero validation errors and no live objects.

## Measurement

- Use RelWithDebInfo or Release with `--no-debug`.
- The GPU is an **RTX 3080 Laptop GPU (GA104, 16 GB)**, not a desktop RTX 3080 (GA102).
- Report driver, CPU, resolution, API, configuration, vsync state and laptop power mode.
- GPU time comes from one timestamp pair around the frame and lags by the frames in flight.

No profiler is installed. PIX, Nsight Graphics or RenderDoc are needed before any bottleneck claim.

## Not installed yet

| Milestone | Needs |
|---|---|
| M3 | Vulkan SDK; `VULKAN_SDK` is unset |
| M5 | COLMAP, PyTorch, gsplat. A Store Python 3.13 is present, but `python` on PATH resolves to a CGRU wrapper |
| Profiling | PIX, Nsight Graphics, RenderDoc |

Around 84 GB free at time of writing; reconstruction datasets are large.

## Troubleshooting

**`UnsupportedFeature` about Enhanced Barriers.** The device or runtime does not report them. Check
the `Device: runtime =` line; if it says "operating system runtime", the `D3D12SDKVersion` /
`D3D12SDKPath` exports or the `D3D12/` directory beside the executable are wrong.

**`cannot read compiled shader ... .dxil`.** The `r4d_shaders` target did not run or the post-build
staging did not copy `build/windows-d3d12/shaders-dxil`. Rebuild.

**Ninja preset cannot find the compiler.** Run it from a developer command prompt.
