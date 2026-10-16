# Reconstruction tooling

Turns a photogrammetry reconstruction into something the engine renders. Pure
Python, no dependencies: `python -m r4drecon` from this folder.

## What works now

```
python -m r4drecon inspect  <sparse-model-folder>
python -m r4drecon export   <sparse-model-folder> <out.ply>
```

`inspect` reports a COLMAP sparse model: cameras and their models, image count
and the spread of the 3D points with their reprojection error and track length.

`export` writes the model's points as a 3D Gaussian Splatting `.ply` that
`--splat-ply` loads. Each point becomes one splat: its position and colour, a
fixed low opacity, an identity rotation, and an isotropic scale taken from the
distance to its three nearest neighbours, the same initialisation the
reference implementation optimises from. That makes a sparse reconstruction
viewable before any training has run.

The neighbour search uses a uniform grid rather than comparing every pair, so
it is linear in the point count: 28,000 points take about 8 seconds.

Only COLMAP's binary model is read. A text model converts with
`colmap model_converter --output_type BIN`.

Nothing here changes coordinate frames. COLMAP's world is right-handed with
+y down, the engine's is left-handed with +y up, and the mirror between them
lives in `src/scene/GaussianPlyLoader.cpp` so that a file written here and a
file downloaded from a published scene are treated identically.

## What is not here yet

`python -m r4drecon train` reports what it would need and exits. Pose
estimation and optimisation need three things, none of them installed on this
machine:

| Tool | Why | Where |
|---|---|---|
| COLMAP | feature extraction, matching, sparse and dense reconstruction | <https://github.com/colmap/colmap/releases> |
| PyTorch with CUDA | the optimisation itself | <https://pytorch.org>, the wheels bundle their own CUDA runtime |
| gsplat | the differentiable splat rasteriser | <https://github.com/nerfstudio-project/gsplat>, builds CUDA extensions, so it needs the CUDA toolkit and MSVC too |

This machine has Python 3.13.14 and the CUDA 13.0 toolkit, and about 58 GB
free on C:. gsplat compiles against the toolkit, so the toolkit version and
the version PyTorch was built against have to agree; check that before
installing rather than after.

## Tests

```
python -m unittest discover -s tests -t .
```

They run as `Reconstruct_test` in the main `ctest` suite. The COLMAP readers
are tested against bytes the tests write themselves, in the same layout
COLMAP's writer produces, including the variable-length names and tracks that
a mis-advancing reader would desynchronise on. The `.ply` writer is tested by
round-tripping the activations the format stores, a logit, two logarithms and
an offset from mid grey, and by asserting the property names the engine's
loader requires.
