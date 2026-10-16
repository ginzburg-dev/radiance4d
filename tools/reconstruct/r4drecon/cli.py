"""The reconstruction command line.

What is here runs with nothing installed beyond Python: reading a COLMAP
sparse model, reporting it, and exporting it as a `.ply` the engine renders.
Pose estimation and training are not here yet, and the commands that will need
COLMAP, PyTorch and gsplat say what to install rather than failing obscurely.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

from . import colmap, gaussians, sparse


def _report_model(model: colmap.SparseModel) -> None:
    print(f"cameras  {len(model.cameras)}")
    for camera in list(model.cameras.values())[:4]:
        focal = camera.focal
        print(f"  [{camera.id}] {camera.model} {camera.width}x{camera.height} "
              f"focal {focal[0]:.1f},{focal[1]:.1f}")
    if len(model.cameras) > 4:
        print(f"  ... and {len(model.cameras) - 4} more")

    print(f"images   {len(model.images)}")
    print(f"points   {len(model.points)}")

    if model.points:
        for axis, name in enumerate("xyz"):
            values = sorted(point.xyz[axis] for point in model.points)
            median = values[len(values) // 2]
            low = values[len(values) // 20]
            high = values[-len(values) // 20 - 1]
            print(f"  {name}: median {median:+.3f}, 5-95% {low:+.3f} .. {high:+.3f}, "
                  f"full {values[0]:+.3f} .. {values[-1]:+.3f}")

        errors = sorted(point.error for point in model.points)
        tracks = sorted(point.track_length for point in model.points)
        print(f"  reprojection error: median {errors[len(errors) // 2]:.3f} px")
        print(f"  track length: median {tracks[len(tracks) // 2]}")


def command_inspect(args: argparse.Namespace) -> int:
    model = colmap.read_model(args.model)
    _report_model(model)
    return 0


def command_export(args: argparse.Namespace) -> int:
    model = colmap.read_model(args.model)
    if not model.points:
        print(f"{args.model}: the model has no 3D points to export",
              file=sys.stderr)
        return 1

    splats = sparse.initial_gaussians(model.points, opacity=args.opacity)
    gaussians.write_ply(args.output, splats)

    scales = sorted(splat.scale[0] for splat in splats)
    print(f"wrote {len(splats)} splats to {args.output}")
    print(f"  scale: median {scales[len(scales) // 2]:.4f}, "
          f"5-95% {scales[len(scales) // 20]:.4f} .. "
          f"{scales[-len(scales) // 20 - 1]:.4f}")
    print(f"  opacity {args.opacity}, identity rotation, degree-0 colour")
    print("  frame: COLMAP's, which is what the engine's loader mirrors")
    return 0


def command_train(args: argparse.Namespace) -> int:
    del args
    print("Training is not implemented yet. It will need, and this machine has "
          "none of:\n"
          "  COLMAP          https://github.com/colmap/colmap/releases "
          "(for pose estimation)\n"
          "  PyTorch + CUDA  https://pytorch.org (the wheels bundle their own "
          "CUDA runtime)\n"
          "  gsplat          https://github.com/nerfstudio-project/gsplat "
          "(builds CUDA extensions, so it also needs the toolkit and MSVC)\n"
          "Until then, `export` produces a renderable starting point from a "
          "sparse model.", file=sys.stderr)
    return 2


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="r4drecon",
        description="Reconstruction tooling for Radiance4D.")
    commands = parser.add_subparsers(dest="command", required=True)

    inspect = commands.add_parser(
        "inspect", help="report what a COLMAP sparse model contains")
    inspect.add_argument("model", type=Path,
                         help="folder holding cameras.bin, images.bin, points3D.bin")
    inspect.set_defaults(handler=command_inspect)

    export = commands.add_parser(
        "export", help="write a sparse model as a .ply the engine can render")
    export.add_argument("model", type=Path, help="COLMAP sparse model folder")
    export.add_argument("output", type=Path, help="destination .ply")
    export.add_argument("--opacity", type=float, default=0.1,
                        help="starting opacity for every splat (default 0.1)")
    export.set_defaults(handler=command_export)

    train = commands.add_parser("train", help="not implemented; says what it needs")
    train.set_defaults(handler=command_train)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        return args.handler(args)
    except colmap.ColmapError as error:
        print(error, file=sys.stderr)
        return 1
    except OSError as error:
        print(f"{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
