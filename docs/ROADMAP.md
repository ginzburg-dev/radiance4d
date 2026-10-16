# Roadmap

Each milestone is independently verifiable. Current position: M3 complete and M4 under way — the
splat pass, `.ply` datasets, spherical-harmonic colour and the GPU sort are in; culling and binning
are not.

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

Not implemented in M1: depth buffer, backface culling, custom allocator, per-pass GPU timings (the
timings landed in M4).

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
scene heavy enough for the numbers to mean something, and a demo video. In-engine per-pass
timestamps landed in M4 and cover the splat path; they are not a substitute for a real tool.

## M4 — Static Gaussian splatting

Done: GPU attribute storage, 3D covariance projection to a screen-space conic, alpha compositing
back to front, a GPU depth sort, a procedural cloud for development and tests, a shared depth
buffer with the mesh pass, both backends drawing it from the same shaders, `.ply` dataset loading,
degree-3 spherical-harmonic colour evaluated per splat from the view direction, and per-pass GPU
timestamps across the splat path on both backends.

Visibility culling and tile-based binning were left until last, because neither can be honestly
measured against 20k procedural splats; both are now done against the million-splat workload a real
dataset provides, and what each one bought is recorded further down.

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

The scene also put a number on why the remaining work is the remaining work, and the sort has since
been answered. At 1920x1080 the frame cost 96.1 ms of CPU against 35.3 ms of GPU on D3D12 (99.7 and
32.4 on Vulkan): the full depth sort of 1.03M splats, single-threaded, every frame.

That sort now runs on the GPU — five compute kernels, four passes over an 8-bit digit of a 32-bit
key, one HLSL source emitted as both DXIL and SPIR-V. A radix sort rather than a bitonic one
because the work is linear in the splat count rather than n log^2 n, which is what the 6.13M-splat
stress test will need. The key is the whole float depth under a monotonic bit transform rather than
a quantised range, so the order is exact, and the sort is stable, so equal depths keep their buffer
order: on `train` the GPU order reproduces the CPU sort it replaced with 0 of 921,600 pixels
differing at 1280x720, on both backends, and backend parity is unchanged at max channel delta 23
and RMSE 0.6806. No test tolerance was changed.

At 1920x1080 the frame is now 22.475 ms of CPU and 22.455 ms of GPU on D3D12, 18.707 and 18.677 on
Vulkan. The CPU figure sitting within 0.03 ms of the GPU one is the point: the CPU is waiting on
the GPU rather than sorting, and the frame is GPU-bound for the first time. The GPU figure fell too,
mostly because the order buffer left the upload heap and the vertex shader stopped fetching a `uint`
per instance across PCIe; a control at 20,000 splats, where neither build is CPU-bound, puts the two
within 0.062 ms, so that is work removed and not a clock artefact.

Visibility culling has since been answered too, in the pass that was already there. Building the
sort key means visiting every splat, so that pass now also projects it once and writes a 48-byte
screen-space record — centre, radius, conic, opacity and the evaluated spherical-harmonic colour —
which the vertex shader reads instead of rebuilding the covariance and the degree-3 sum at each of
the quad's four corners. A splat whose quad cannot reach a pixel is collapsed to a zero radius.
`train` at 1080p went from 22.455 ms of GPU to 14.846 on D3D12 and 18.677 to 12.041 on Vulkan.

The rejection is exact rather than a heuristic, and that was measured rather than argued: with the
viewport test removed the capture is identical, differing from the pre-change one in the same 2,619
pixels of 921,600, which are floating-point and not geometry. The stored WARP reference passes
untouched.

Driving the draw indirectly from a GPU-side survivor count was implemented and then removed. It
measured 14.846 ms against 14.530 for collapsing the culled splats, which is inside the run-to-run
spread, and the pinned WARP draws nothing through `ExecuteIndirect` — reproduced with the argument
buffer filled by the CPU, so the compute write was not the cause. A blind spot in the only
GPU-independent test path was not worth paying for no measured gain.

Tile-based binning has since replaced that draw, which closes M4's list. The splats are binned into
16x16 tiles, and a compute group per tile composites its own pixels from the nearest splat outwards,
stopping when the transmittance can no longer change them — the early exit a back-to-front blend
cannot have. `train` at 1080p went from 14.846 ms of GPU to 9.900 on D3D12 and 12.041 to 10.408 on
Vulkan; against the quad pass before culling, 22.455 to 9.900.

The binning needs no second sort key. Pairs are generated by walking the splats in depth order, so
they arrive sorted by depth, and sorting them by tile id with the same stable radix sort leaves
every tile's slice in that order. Two passes cover a 16-bit tile id, against the eight a packed
(tile, depth) key would need.

Pair capacity starts at eight per splat — 8,212,224 slots for `train` — and the grand total is read
back. How much of it a view fills is a property of the camera, not the scene: 4,547,915 at 1280x720
with `--orbit-yaw 150`, 7,571,308 for the same view at 1080p, and 8,902,955 for the default 1080p
framing the benchmark rows use. A yaw sweep at 1080p puts the worst view at `--orbit-yaw 30` and
9,966,714 pairs, so the default framing overflowed but was never the worst case. What a view that
wants more does now is below.

The stored WARP reference moved with it. The quad pass blended into the 8-bit target once per splat,
so overlapping splats accumulated a rounding each; the rasteriser accumulates in floating point and
writes once. Retaking the reference records 2787 of 230400 pixels moving by at most 9 levels, and
cross-backend agreement improved from max channel delta 2 over 0.49% of pixels to delta 1 over
0.046%.

Per-pass GPU timestamps now separate those costs. Six scopes bracket the splat path — cull, depth
sort, binning, tile sort, rasterise, composite — with the sixteen radix dispatches grouped into two
entries, on both backends, reported in the ImGui overlay and in the `--benchmark` report, and
switchable off with `--no-pass-timings`. On `train` at 1920x1080 over 200 measured frames:

| Pass | D3D12 | Vulkan |
|---|---|---|
| Cull | 0.91 ms | 0.74 ms |
| Depth sort | 0.81 ms | 0.74 ms |
| Binning | 1.38 ms | 1.27 ms |
| Tile sort | 2.74 ms | 4.00 ms |
| Rasterise | 3.82 ms | 3.59 ms |
| Composite | 0.04 ms | 0.04 ms |
| sum of the six scopes | 9.69 ms | 10.37 ms |
| whole-frame pair | 9.70 ms | 10.38 ms |

The scopes sum to the whole-frame pair within 0.01 ms. The rasteriser is the largest single pass at
39% of the D3D12 frame, but the depth sort, binning and tile sort together are 51% of it, and the
tile sort alone costs more than the depth sort that feeds it: the remaining cost is in getting the
splats into tiles. The one place the backends disagree is that tile sort, 4.00 ms on Vulkan against
2.74 on D3D12, which is more than the 0.68 ms by which the whole Vulkan frame is dearer — Vulkan is
cheaper in all five other passes. The same kernels run on both, so that gap is a dispatch or barrier
difference around the pair buffers, and it is the next thing to chase.

The scopes are not paid for in frame time: paired runs with and without `--no-pass-timings`, interleaved three
times per backend, differ by 0.02 ms of frame time against a 0.3 ms run-to-run spread, so the twelve
extra timestamps are not measurable. A sampling profiler is still outstanding; this attributes time
per pass, not per instruction.

Those scopes made the tile sort answerable, and it has since been bounded. The histogram, the row
scan, the scatter and the per-tile ranges all walked the whole pair capacity, sorting filler whose
only job was to sort last; each now stops at the total the binning scan already computes on the GPU,
rounded up to a whole group. The window is read on the GPU rather than predicted from the readback,
because the pair total moves by millions between views and a dispatch sized from a stale one would
drop splats whenever it grew. At 1280x720 with `--orbit-yaw 150`, where 4.55 of 8.21 million slots
are filled, the tile sort falls from 2.401 ms to 1.542 on D3D12 and 2.836 to 1.980 on Vulkan, and
the frame from 7.941 to 7.013 and 7.845 to 6.938; binning drops 0.07 ms with it, because the filler
was eight million stores nothing reads. The four untouched passes move by at most 0.012 ms, and the
image is byte-identical on both backends.

The published 1080p row does not move, and that is the result worth keeping: its view wants 8.90
million pairs against 8.21 million held, so it has no empty slots to skip — 3.139 ms against 3.163
on D3D12, inside the spread. The pass now costs what the view needs rather than what the capacity
allows, which makes capacity a memory decision instead of a frame-time one: doubling it to sixteen
per splat at the 720p view cost 2.08 ms of tile sort before and costs 0.143 ms now, and on the
overflowing 1080p view sixteen stops dropping splats for 0.39 ms instead of 2.10. It was left at
eight there because sixteen doubles the pair buffers; that decision is settled below. Indirect
dispatch would remove the groups that
still launch before retiring — 0.14 ms on D3D12 and 0.64 on Vulkan at sixteen per splat, which is
what those figures measure — and is not worth an argument buffer, a root-signature change on both
backends and an unverified path on the WARP the image test runs on until that residual matters.

Pair capacity now follows demand, which ends the dropping. The overflow was never a fair sample of
the scene: pairs are laid out in depth order, so what falls past the end of the buffer is the near
end of that order, and the default 1080p `train` view had been rendering without its closest 691k
pairs' worth of splats since binning landed. Vulkan had no readback for the total at all and so
never reported it.

Raising the per-splat multiple is aimed the wrong way, and `truck` is what settles it: at 2.5x
`train`'s splats it wants 6.14M to 7.25M pairs at 1080p against `train`'s 8.90M to 9.97M, because
demand follows how much screen the splats cover rather than how many there are. Eight per splat
already leaves `truck` holding 20.3M slots it cannot use while `train` overflows at 8.21M, and
doubling would take it to 40.7M — 1.24 GiB of pair buffers for a 7.25M need. Clamping the tile span
harder does not reach either: reading the per-splat counts back shows a median span of 4 tiles and
a 90th percentile of 16, but a heavy tail in which 8,127 splats spanning 129 to 1024 tiles hold 31%
of the pairs, so fitting 8.21M needs the clamp at 256, truncating 5,094 splats at the worst view
for 0.8% of headroom — and truncating a splat's box in row order leaves a hard edge through the
largest splats on screen.

So the capacity is a guess only until the first frame corrects it. The binning scan already
computes the exact total, and that total is right even for a frame that overflowed, because the
counts it sums are never clamped. A frame wanting more grows the four pair buffers and the shared
histogram to it plus a quarter, once, with the queue drained; past thirty-two per splat the view is
refused and reported instead. `train` at 1080p grows 8,212,224 to 11,128,832 (313.3 to 424.5 MiB
across both frame slots) and never again, 3840x2160 grows once to 29,810,176 (1137.2 MiB), and
`truck` at 1080p never grows. The growth costs 48.9–53.4 ms on D3D12 and 31.2–31.8 on Vulkan, most
of it the queue drain, inside the benchmark's warm-up.

| Pass | D3D12 before | D3D12 after | Vulkan before | Vulkan after |
|---|---|---|---|---|
| Cull | 0.907 ms | 0.911 ms | 0.744 ms | 0.748 ms |
| Depth sort | 0.831 ms | 0.830 ms | 0.747 ms | 0.748 ms |
| Binning | 1.171 ms | 1.457 ms | 1.045 ms | 1.331 ms |
| Tile sort | 2.789 ms | 3.064 ms | 4.011 ms | 4.403 ms |
| Rasterise | 4.123 ms | 4.420 ms | 3.858 ms | 4.126 ms |
| Composite | 0.035 ms | 0.033 ms | 0.034 ms | 0.033 ms |
| whole-frame pair | 9.865 ms | 10.724 ms | 10.451 ms | 11.399 ms |

The frame costs 0.86 ms more on D3D12 and 0.95 on Vulkan, all of it in the three passes with more
to do: binning writes 691k more pairs, the tile sort sorts them — and since it is already bounded
by the real total, that is the cost of the pairs rather than of the larger buffers — and the
rasteriser composites the splats that were missing. The other three move by at most 0.004 ms. At
1280x720 `--orbit-yaw 150`, which neither overflows nor grows, every pass moves by at most 0.018 ms
and the frame by −0.016 and −0.022: the change costs nothing where it is not needed.

The 1080p image changed on 73.7% of its pixels and it looked worse, which was the honest result.
The splats the overflow dropped are the nearest, and the nearest here are the huge-scale ones that
produce the streaking already noted: seen from inside the scene they smear across the frame. The
overflow had been an accidental declutter, and removing it exposed a defect that was always there.
The output became byte-identical, on both backends, to a build given capacity to spare, so the fix
was right. The 1280x720 `--orbit-yaw 150` screenshot was verified byte-identical on both backends
rather than assumed, and the stored WARP reference did not move — `SplatRender_test` renders small
enough never to reach its capacity.

The cull pass then took on the streaking, and makes two rejections it was not making before.

The first is exact and was simply missing: the rasteriser discards any sample under 1/255 alpha,
and a splat's alpha never exceeds its stored opacity, so a splat below that floor cannot colour a
pixel from any camera. 76,594 of `train`'s 1,026,508 splats are stored under it, and every one was
being projected, sorted, binned, sorted again and read back to be discarded. Output is
byte-identical on both backends because the rejection is exact; `train` at 1080p drops from
8,902,955 pairs to 8,188,478, under the 8,212,224 it starts with, so the default view no longer
grows the pair buffers at all, and `truck` sheds 661,018 of 6,776,386.

The second weights a splat by how much of the Jacobian's linearisation still holds across its own
depth extent. The linearisation drops the terms quadratic in the depth offset and, a Gaussian being
symmetric in depth, the first-order over- and under-magnification cancel, so the error left in the
projected covariance goes as (σ_z / z)²; in the three-sigma extent the renderer already uses for
the radius and the quad, that ratio is 1 exactly when the near end of the extent reaches the eye
plane. Weighting by what survives costs a splat a tenth of its depth away one per cent and takes
the ones the camera is inside to zero, with both ends set by that convention rather than chosen.
It was preferred to a world-scale rejection at load, which cannot see that the fifty worst splats
in the default view are 64.7 px and perfectly ordinary from the screenshot view and does not
transfer between scenes (a ceiling costing `train` 6.5% of its pairs costs `truck` 9.1%), and to a
screen-radius ceiling, which cuts by the symptom; the fade is smooth in the camera's position, and
over a 0.05° yaw sweep the step-to-step change falls from 1.77–1.87 to 1.42–1.44 levels.

Together they take the 1080p frame from 10.696 to 9.785 ms on D3D12 and 11.374 to 10.466 on
Vulkan, which returns the 0.86 and 0.95 ms that sizing the pairs to demand had cost; the composite
is the control at ±0.000 ms. 720p `--orbit-yaw 150` goes 7.006 to 6.321 and 6.918 to 6.307. The
screenshot moves this time and improves — 72.4% of pixels at a mean of 1.8 levels, the haze over
the sky lifting — and was retaken; cross-backend agreement on it goes from max delta 2 / RMSE
0.0447 over 0.49% of pixels to delta 1 / RMSE 0.0121 over 0.04%. The WARP reference moved by one
level over 1,592 of 230,400 pixels, inside the tolerance the test already allowed, and was retaken
so it stays exact.

**It does not make the default view legible, and that is the part worth recording.** The fade
removes 13.0% of the ink laid in front of the subject there, and that 13% is the frame-wide blobs.
The rest is not reachable by any rejection rule. Compositing the nearest 40,000 splats front to
back as the rasteriser does, they block 75.6% of the frame, and the ink needs 6 splats for the
first quarter but 47 for half, 302 for three quarters and 1,134 for 90% — a long tail, not a
handful. 39% of it comes from splats under a tenth of the screen diagonal in radius, the size the
working view's foreground is made of, and a probe cutting at that tenth removes 834 splats and 60%
of the near-field ink and still leaves the frame unreadable. What is in the way is 34,658 splats of
ordinary opaque geometry standing between the camera and the locomotive, against 6,487 at the
screenshot view; culling everything nearer than the subject renders the scene cleanly, which proves
nothing behind them is wrong, but that is a world distance picked for one camera and it would
delete the foreground of the view that works. `--orbit-yaw 150` remains the documented way to aim
at this scene, and `truck`'s default framing fails the same way for the same reason.

One incidental finding: moving those buffer allocations to the end of the Vulkan upload sequence,
which is where a growable helper naturally puts them, cost 0.497 ms of tile sort at 720p with no
change in work at all. Keeping them in the position they had restores it exactly. Vulkan gives each
buffer its own allocation here, and where one lands in the sequence is apparently not free.

A second published scene has been through the same path to see how it scales: INRIA's `truck`,
2,541,226 splats at SH degree 3, 581.6 MiB of GPU attributes and 96.9 MiB of sort scratch, zero
validation errors on both backends. Parse and convert takes 1.17 s and the upload 0.15 s, 2.48x and
2.32x `train` for 2.48x the splats, so loading and memory are both linear. Nothing degrades: a
procedural 4M-splat cloud (915.5 MiB of attributes, 152.6 MiB of scratch) also runs clean on both
backends.

The frame time does not scale with the splat count, and that is the useful result. At 1920x1080
`truck` costs 14.698 ms of GPU against `train`'s 22.050 ms on D3D12 — 2.5x the splats, two thirds
of the frame — because the frame is bounded by rasterised area rather than by per-splat work. CPU
stays within 0.03 ms of GPU on both backends, so nothing has moved back onto the CPU. Shrinking the
target to 160x90 to strip the fill leaves the view-independent half: D3D12 goes 2.472 ms to 6.123 ms
for the 2.476x splat count, linear to within 0.1%, while Vulkan goes 2.182 ms to 7.217 ms. A
procedural sweep holding the data fixed confirms it: Vulkan is the cheaper backend to a little over
2M splats and the dearer one above that, and from 1M to 4M it rises 4.40x against D3D12's 3.87x.
Those runs predate the per-pass scopes and have not been re-run with them, so which pass bends at
those counts is still open; at 1080p on `train` it is the tile sort that separates the backends.

That makes the ordering of the remaining work concrete. Fill, not splat count, sets the frame, which
is the argument for tiled binning; and the per-splat half being linear is what says the sort is no
longer the thing to optimise.

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
backends, D3D12 against Vulkan at max channel delta 1 / RMSE 0.0121 over 0.04% of pixels, and
byte-identical between two runs of the same command. It was retaken when the cull pass began
fading splats by their projection's validity, which is further up this section.

`truck` reproduces the first of those and adds a third. Its un-clamped capture is a featureless
white field — every pixel between 253 and 255 on all three channels — so the D3D12-against-Vulkan
figure measured on it is two blank images agreeing and means nothing; with the clamp applied as a
probe the scene renders and parity is max channel delta 2, RMSE 0.0380 over 3,360 of 921,600 pixels.
The third is the framing heuristic itself. `truck`'s 251 capture cameras sit a median 4.12 and at
most 6.40 units from the centre, but half its splats lie 12.5 units or further out, so the framing
sphere lands at radius 12.6 and the camera 28 units back — inside the reconstructed background
rather than where the scene was photographed from. The median distance measures how much background
a scene reconstructed, not where its cameras stood; reading the capture poses, when a dataset ships
them, would.

## M5 — Reconstruction pipeline

COLMAP for pose estimation, PyTorch and gsplat for optimisation, a Python CLI for dataset processing
and training, export into the runtime.

Done: the half that needs nothing installed. `tools/reconstruct` is a dependency-free Python package
that reads COLMAP's binary sparse model — cameras, images and 3D points — and writes the points as a
3DGS `.ply` the engine loads. Each point becomes a splat with its colour, a fixed opacity, an
identity rotation and an isotropic scale from its three nearest neighbours, which is what the
reference implementation starts optimisation from; a sparse reconstruction is therefore viewable
before any training. The neighbour search uses a uniform grid, so it is linear in the point count
rather than quadratic: 28,000 points take about 8 seconds.

The readers are tested against bytes the tests write in COLMAP's own layout, including the
variable-length image names and point tracks that a mis-advancing reader would desynchronise on, and
the writer is tested by round-tripping the activations the format stores. They run in the main
suite as `Reconstruct_test`. End to end, a synthesised 28,000-point model exports and renders with
zero validation errors.

Remaining: pose estimation and optimisation, which need COLMAP, PyTorch with CUDA and gsplat. None
is installed on this machine, and `r4drecon train` says so and exits rather than failing obscurely.
Only the binary COLMAP model is read; a text model is converted with `colmap model_converter`.

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
