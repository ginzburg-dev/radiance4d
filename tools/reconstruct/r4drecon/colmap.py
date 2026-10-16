"""Readers for a COLMAP sparse model.

Only the binary form is read. COLMAP writes it by default and published
datasets ship it; a text model is converted with `colmap model_converter
--output_type BIN` rather than parsed here twice.

The structures stay close to COLMAP's own: this module reports what the file
says and converts nothing, so that every convention change happens in one
place further along.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path


class ColmapError(Exception):
    """A model that cannot be read, with the file and the reason."""


# Model id to (name, parameter count), from COLMAP's src/colmap/sensor/models.h.
CAMERA_MODELS = {
    0: ("SIMPLE_PINHOLE", 3),
    1: ("PINHOLE", 4),
    2: ("SIMPLE_RADIAL", 4),
    3: ("RADIAL", 5),
    4: ("OPENCV", 8),
    5: ("OPENCV_FISHEYE", 8),
    6: ("FULL_OPENCV", 12),
    7: ("FOV", 5),
    8: ("SIMPLE_RADIAL_FISHEYE", 4),
    9: ("RADIAL_FISHEYE", 5),
    10: ("THIN_PRISM_FISHEYE", 12),
}


@dataclass(frozen=True)
class Camera:
    id: int
    model: str
    width: int
    height: int
    params: tuple[float, ...]

    @property
    def focal(self) -> tuple[float, float]:
        """Focal length in pixels, for the models that start with one or two."""
        if self.model in ("SIMPLE_PINHOLE", "SIMPLE_RADIAL", "RADIAL",
                          "SIMPLE_RADIAL_FISHEYE", "RADIAL_FISHEYE", "FOV"):
            return (self.params[0], self.params[0])
        return (self.params[0], self.params[1])


@dataclass(frozen=True)
class Image:
    id: int
    # World-to-camera, as COLMAP stores it: quaternion w,x,y,z then translation.
    rotation: tuple[float, float, float, float]
    translation: tuple[float, float, float]
    camera_id: int
    name: str


@dataclass(frozen=True)
class Point3D:
    id: int
    xyz: tuple[float, float, float]
    rgb: tuple[int, int, int]
    error: float
    track_length: int


@dataclass(frozen=True)
class SparseModel:
    cameras: dict[int, Camera]
    images: dict[int, Image]
    points: list[Point3D]

    def __len__(self) -> int:
        return len(self.points)


class _Reader:
    """A cursor over the file, so a truncated model names the field that ran out."""

    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self.offset = 0

    def take(self, fmt: str, what: str):
        size = struct.calcsize(fmt)
        if self.offset + size > len(self.data):
            raise ColmapError(
                f"{self.path}: ran out of data reading {what} at byte {self.offset}")
        values = struct.unpack_from(fmt, self.data, self.offset)
        self.offset += size
        return values

    def take_string(self, what: str) -> str:
        end = self.data.find(b"\x00", self.offset)
        if end < 0:
            raise ColmapError(f"{self.path}: unterminated {what} at byte {self.offset}")
        text = self.data[self.offset:end].decode("utf-8", errors="replace")
        self.offset = end + 1
        return text

    def finished(self) -> bool:
        return self.offset == len(self.data)


def read_cameras(path: Path) -> dict[int, Camera]:
    reader = _Reader(path)
    (count,) = reader.take("<Q", "camera count")

    cameras: dict[int, Camera] = {}
    for _ in range(count):
        camera_id, model_id, width, height = reader.take("<iiQQ", "camera header")
        if model_id not in CAMERA_MODELS:
            raise ColmapError(f"{path}: unknown camera model id {model_id}")
        name, parameter_count = CAMERA_MODELS[model_id]
        params = reader.take(f"<{parameter_count}d", f"{name} parameters")
        cameras[camera_id] = Camera(camera_id, name, width, height, params)

    return cameras


def read_images(path: Path) -> dict[int, Image]:
    reader = _Reader(path)
    (count,) = reader.take("<Q", "image count")

    images: dict[int, Image] = {}
    for _ in range(count):
        values = reader.take("<idddddddi", "image pose")
        image_id = values[0]
        rotation = tuple(values[1:5])
        translation = tuple(values[5:8])
        camera_id = values[8]
        name = reader.take_string("image name")

        # The 2D observations are only needed for bundle adjustment, so they
        # are skipped rather than kept.
        (observations,) = reader.take("<Q", "observation count")
        reader.take("<" + "ddq" * observations, "observations")

        images[image_id] = Image(image_id, rotation, translation, camera_id, name)

    return images


def read_points(path: Path) -> list[Point3D]:
    reader = _Reader(path)
    (count,) = reader.take("<Q", "point count")

    points: list[Point3D] = []
    for _ in range(count):
        values = reader.take("<QdddBBBd", "point record")
        (track_length,) = reader.take("<Q", "track length")
        reader.take("<" + "ii" * track_length, "track")

        points.append(Point3D(id=values[0],
                              xyz=tuple(values[1:4]),
                              rgb=tuple(values[4:7]),
                              error=values[7],
                              track_length=track_length))

    return points


def read_model(directory: Path) -> SparseModel:
    """Reads cameras.bin, images.bin and points3D.bin from one sparse folder."""
    directory = Path(directory)
    missing = [name for name in ("cameras.bin", "images.bin", "points3D.bin")
               if not (directory / name).is_file()]
    if missing:
        raise ColmapError(
            f"{directory}: not a COLMAP sparse model, missing {', '.join(missing)}. "
            "A text model is converted with "
            "`colmap model_converter --output_type BIN`.")

    return SparseModel(cameras=read_cameras(directory / "cameras.bin"),
                       images=read_images(directory / "images.bin"),
                       points=read_points(directory / "points3D.bin"))
