"""The 3D Gaussian Splatting `.ply` format, written the way the engine reads it.

Field names, order and activations match `src/scene/GaussianPlyLoader.cpp`:
positions are stored raw, opacity as a logit, scales as logarithms and the
rotation as a `wxyz` quaternion. Colour is the degree-0 spherical-harmonic
band, which is an offset from mid grey rather than the colour itself.

Nothing here changes coordinate frames. A file written from a COLMAP model
stays in COLMAP's frame, which is what trained scenes ship in and what the
loader's mirror expects.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from pathlib import Path

# The degree-0 basis function, so a colour round-trips through f_dc.
SH_C0 = 0.28209479177387814


def colour_to_dc(channel: float) -> float:
    """0..1 colour to the degree-0 coefficient the shader reconstructs it from."""
    return (channel - 0.5) / SH_C0


def dc_to_colour(coefficient: float) -> float:
    return coefficient * SH_C0 + 0.5


def inverse_sigmoid(value: float) -> float:
    """Opacity to the logit the file stores. Clamped off the asymptotes."""
    value = min(max(value, 1e-6), 1.0 - 1e-6)
    return math.log(value / (1.0 - value))


@dataclass
class Gaussian:
    position: tuple[float, float, float]
    colour: tuple[float, float, float] = (0.5, 0.5, 0.5)
    opacity: float = 0.1
    scale: tuple[float, float, float] = (0.01, 0.01, 0.01)
    rotation: tuple[float, float, float, float] = (1.0, 0.0, 0.0, 0.0)


def property_names(rest_coefficients: int = 0) -> list[str]:
    """The header's property order, which the writer and the tests share."""
    names = ["x", "y", "z", "nx", "ny", "nz"]
    names += [f"f_dc_{i}" for i in range(3)]
    names += [f"f_rest_{i}" for i in range(rest_coefficients)]
    names += ["opacity"]
    names += [f"scale_{i}" for i in range(3)]
    names += [f"rot_{i}" for i in range(4)]
    return names


def write_ply(path: Path, gaussians: list[Gaussian]) -> None:
    """Writes a binary little-endian file with degree-0 colour only.

    The normals are zero: the reference exporter emits them and some readers
    insist on them, while the engine ignores them.
    """
    path = Path(path)
    names = property_names()

    header = ["ply", "format binary_little_endian 1.0",
              f"element vertex {len(gaussians)}"]
    header += [f"property float {name}" for name in names]
    header += ["end_header", ""]

    with path.open("wb") as file:
        file.write("\n".join(header).encode("ascii"))

        record = struct.Struct(f"<{len(names)}f")
        for g in gaussians:
            file.write(record.pack(
                g.position[0], g.position[1], g.position[2],
                0.0, 0.0, 0.0,
                colour_to_dc(g.colour[0]), colour_to_dc(g.colour[1]),
                colour_to_dc(g.colour[2]),
                inverse_sigmoid(g.opacity),
                math.log(g.scale[0]), math.log(g.scale[1]), math.log(g.scale[2]),
                g.rotation[0], g.rotation[1], g.rotation[2], g.rotation[3]))


def read_ply(path: Path) -> list[Gaussian]:
    """Reads back what `write_ply` wrote, with the activations undone.

    Enough for the tests and for `inspect`; the engine has its own reader, and
    this one deliberately does not grow to match every exporter.
    """
    path = Path(path)
    data = path.read_bytes()

    marker = b"end_header\n"
    end = data.find(marker)
    if end < 0:
        raise ValueError(f"{path}: no end_header")
    header = data[:end].decode("ascii", errors="replace").splitlines()
    body = data[end + len(marker):]

    if not header or header[0].strip() != "ply":
        raise ValueError(f"{path}: not a ply file")
    if "format binary_little_endian 1.0" not in header:
        raise ValueError(f"{path}: only binary little-endian is read here")

    count = 0
    names: list[str] = []
    for line in header:
        if line.startswith("element vertex "):
            count = int(line.split()[2])
        elif line.startswith("property "):
            parts = line.split()
            if parts[1] != "float":
                raise ValueError(f"{path}: only float properties are read here")
            names.append(parts[2])

    index = {name: i for i, name in enumerate(names)}
    for required in ("x", "opacity", "scale_0", "rot_0", "f_dc_0"):
        if required not in index:
            raise ValueError(f"{path}: no '{required}' property")

    record = struct.Struct(f"<{len(names)}f")
    if len(body) < count * record.size:
        raise ValueError(f"{path}: body holds {len(body)} bytes, "
                         f"{count * record.size} expected")

    gaussians: list[Gaussian] = []
    for i in range(count):
        v = record.unpack_from(body, i * record.size)
        gaussians.append(Gaussian(
            position=(v[index["x"]], v[index["y"]], v[index["z"]]),
            colour=tuple(dc_to_colour(v[index[f"f_dc_{c}"]]) for c in range(3)),
            opacity=1.0 / (1.0 + math.exp(-v[index["opacity"]])),
            scale=tuple(math.exp(v[index[f"scale_{c}"]]) for c in range(3)),
            rotation=tuple(v[index[f"rot_{c}"]] for c in range(4))))

    return gaussians
