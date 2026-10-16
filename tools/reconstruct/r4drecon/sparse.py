"""Turns a COLMAP sparse point cloud into an initial set of Gaussians.

This is what 3DGS starts optimisation from, and on its own it is already
something the engine can display: the reconstruction's geometry and colour,
before any training has run.

Each splat takes its position and colour from the point, a fixed low opacity,
an identity rotation, and an isotropic scale from how far its neighbours are.
That last one needs nearest neighbours over hundreds of thousands of points,
so they come from a uniform grid rather than a full comparison.
"""

from __future__ import annotations

import math
from collections import defaultdict

from .colmap import Point3D
from .gaussians import Gaussian

# The reference implementation averages the three nearest neighbours.
NEIGHBOURS = 3
# A lone point still needs a size; this is the floor on the squared distance.
MIN_SQUARED_DISTANCE = 1e-7


def _cell_size(points: list[Point3D]) -> float:
    """A cell holding a handful of points on average, from the bounding box."""
    if not points:
        return 1.0

    low = [min(p.xyz[i] for p in points) for i in range(3)]
    high = [max(p.xyz[i] for p in points) for i in range(3)]
    extent = [high[i] - low[i] for i in range(3)]
    volume = max(extent[0] * extent[1] * extent[2], 1e-12)

    # Two points per cell on average: a 3x3x3 search then looks at about fifty
    # candidates, which is enough to find three neighbours and four times
    # cheaper than a denser grid.
    return max((volume * 2.0 / len(points)) ** (1.0 / 3.0), 1e-6)


def neighbour_scales(points: list[Point3D]) -> list[float]:
    """Per point, the root-mean-square distance to its nearest neighbours."""
    if len(points) < 2:
        return [math.sqrt(MIN_SQUARED_DISTANCE)] * len(points)

    size = _cell_size(points)
    grid: dict[tuple[int, int, int], list[int]] = defaultdict(list)
    for index, point in enumerate(points):
        key = tuple(int(math.floor(point.xyz[axis] / size)) for axis in range(3))
        grid[key].append(index)

    scales: list[float] = []
    for index, point in enumerate(points):
        base = tuple(int(math.floor(point.xyz[axis] / size)) for axis in range(3))

        nearest: list[float] = []
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    cell = (base[0] + dx, base[1] + dy, base[2] + dz)
                    for other in grid.get(cell, ()):
                        if other == index:
                            continue
                        delta = tuple(points[other].xyz[a] - point.xyz[a]
                                      for a in range(3))
                        nearest.append(delta[0] * delta[0] + delta[1] * delta[1] +
                                       delta[2] * delta[2])

        if nearest:
            nearest.sort()
            taken = nearest[:NEIGHBOURS]
            mean = sum(taken) / len(taken)
        else:
            # An isolated point in a sparse region: the cell size is the only
            # length scale available.
            mean = size * size

        scales.append(math.sqrt(max(mean, MIN_SQUARED_DISTANCE)))

    return scales


def initial_gaussians(points: list[Point3D], opacity: float = 0.1) -> list[Gaussian]:
    """One Gaussian per reconstructed point, sized by its neighbourhood."""
    scales = neighbour_scales(points)

    gaussians: list[Gaussian] = []
    for point, scale in zip(points, scales):
        colour = tuple(channel / 255.0 for channel in point.rgb)
        gaussians.append(Gaussian(position=point.xyz, colour=colour,
                                  opacity=opacity, scale=(scale, scale, scale)))

    return gaussians
