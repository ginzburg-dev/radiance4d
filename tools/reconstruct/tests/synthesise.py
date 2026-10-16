"""Writes COLMAP binary models, so the readers are tested against real bytes.

The layouts here are the mirror image of the readers, taken from COLMAP's own
writer, and the tests compare what comes back rather than how it was parsed.
"""

from __future__ import annotations

import struct
from pathlib import Path


def write_cameras(path: Path, cameras: list[tuple]) -> None:
    """cameras: (id, model_id, width, height, params)."""
    with Path(path).open("wb") as file:
        file.write(struct.pack("<Q", len(cameras)))
        for camera_id, model_id, width, height, params in cameras:
            file.write(struct.pack("<iiQQ", camera_id, model_id, width, height))
            file.write(struct.pack(f"<{len(params)}d", *params))


def write_images(path: Path, images: list[tuple]) -> None:
    """images: (id, qvec, tvec, camera_id, name, observations)."""
    with Path(path).open("wb") as file:
        file.write(struct.pack("<Q", len(images)))
        for image_id, qvec, tvec, camera_id, name, observations in images:
            file.write(struct.pack("<i", image_id))
            file.write(struct.pack("<4d", *qvec))
            file.write(struct.pack("<3d", *tvec))
            file.write(struct.pack("<i", camera_id))
            file.write(name.encode("utf-8") + b"\x00")
            file.write(struct.pack("<Q", len(observations)))
            for x, y, point_id in observations:
                file.write(struct.pack("<ddq", x, y, point_id))


def write_points(path: Path, points: list[tuple]) -> None:
    """points: (id, xyz, rgb, error, track)."""
    with Path(path).open("wb") as file:
        file.write(struct.pack("<Q", len(points)))
        for point_id, xyz, rgb, error, track in points:
            file.write(struct.pack("<Q", point_id))
            file.write(struct.pack("<3d", *xyz))
            file.write(struct.pack("<3B", *rgb))
            file.write(struct.pack("<d", error))
            file.write(struct.pack("<Q", len(track)))
            for image_id, point2d in track:
                file.write(struct.pack("<ii", image_id, point2d))


def write_model(directory: Path, cameras, images, points) -> Path:
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    write_cameras(directory / "cameras.bin", cameras)
    write_images(directory / "images.bin", images)
    write_points(directory / "points3D.bin", points)
    return directory


def simple_model(directory: Path, point_count: int = 8) -> Path:
    """A PINHOLE camera, two images and a lattice of coloured points."""
    cameras = [(1, 1, 640, 480, (500.0, 505.0, 320.0, 240.0))]
    images = [
        (1, (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 1, "frame_0001.jpg",
         [(10.0, 20.0, 1), (30.0, 40.0, 2)]),
        (2, (0.0, 1.0, 0.0, 0.0), (1.0, 2.0, 3.0), 1, "frame_0002.jpg", []),
    ]
    points = []
    for i in range(point_count):
        points.append((i + 1,
                       (float(i), float(i % 3), float(i % 5)),
                       (i * 7 % 256, i * 13 % 256, i * 29 % 256),
                       0.5 + i * 0.01,
                       [(1, i), (2, i)]))

    return write_model(directory, cameras, images, points)
