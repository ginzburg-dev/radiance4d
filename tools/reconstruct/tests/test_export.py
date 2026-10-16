import math
import tempfile
import unittest
from pathlib import Path

from r4drecon import cli, colmap, gaussians, sparse

from . import synthesise


class PlyRoundTrip(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)

    def tearDown(self):
        self._directory.cleanup()

    def test_activations_survive_the_round_trip(self):
        # The file stores a logit, two logarithms and an offset from mid grey;
        # what matters is that the engine's inverse recovers what was meant.
        written = [
            gaussians.Gaussian(position=(1.0, -2.0, 3.0), colour=(0.25, 0.5, 0.95),
                               opacity=0.37, scale=(0.1, 0.02, 0.5),
                               rotation=(1.0, 0.0, 0.0, 0.0)),
            gaussians.Gaussian(position=(0.0, 0.0, 0.0), colour=(1.0, 0.0, 0.5),
                               opacity=0.9, scale=(1.0, 1.0, 1.0),
                               rotation=(0.5, 0.5, 0.5, 0.5)),
        ]
        path = self.root / "round.ply"
        gaussians.write_ply(path, written)

        read = gaussians.read_ply(path)
        self.assertEqual(len(read), len(written))
        for before, after in zip(written, read):
            for i in range(3):
                self.assertAlmostEqual(before.position[i], after.position[i], places=5)
                self.assertAlmostEqual(before.colour[i], after.colour[i], places=5)
                self.assertAlmostEqual(before.scale[i], after.scale[i], places=5)
            self.assertAlmostEqual(before.opacity, after.opacity, places=5)

    def test_header_names_match_the_engines_reader(self):
        # These names are the contract with GaussianPlyLoader.cpp.
        names = gaussians.property_names()
        for required in ("x", "y", "z", "opacity",
                         "scale_0", "scale_1", "scale_2",
                         "rot_0", "rot_1", "rot_2", "rot_3",
                         "f_dc_0", "f_dc_1", "f_dc_2"):
            self.assertIn(required, names)

        path = self.root / "header.ply"
        gaussians.write_ply(path, [gaussians.Gaussian(position=(0.0, 0.0, 0.0))])
        header = path.read_bytes().split(b"end_header")[0].decode("ascii")
        self.assertIn("format binary_little_endian 1.0", header)
        self.assertIn("element vertex 1", header)

    def test_mid_grey_is_a_zero_coefficient(self):
        self.assertAlmostEqual(gaussians.colour_to_dc(0.5), 0.0, places=9)
        self.assertAlmostEqual(gaussians.dc_to_colour(0.0), 0.5, places=9)


class SparseInitialisation(unittest.TestCase):
    def test_scale_follows_neighbour_spacing(self):
        # Two lattices an order of magnitude apart: the splats have to come out
        # that much apart in size, or the initialisation says nothing about the
        # geometry.
        def lattice(step, offset):
            return [colmap.Point3D(id=0, xyz=(offset + x * step, y * step, z * step),
                                   rgb=(128, 128, 128), error=0.0, track_length=2)
                    for x in range(4) for y in range(4) for z in range(4)]

        tight = lattice(0.1, 0.0)
        loose = lattice(1.0, 1000.0)

        tight_scales = sparse.neighbour_scales(tight)
        loose_scales = sparse.neighbour_scales(loose)

        tight_median = sorted(tight_scales)[len(tight_scales) // 2]
        loose_median = sorted(loose_scales)[len(loose_scales) // 2]
        self.assertAlmostEqual(tight_median, 0.1, places=3)
        self.assertAlmostEqual(loose_median, 1.0, places=3)

    def test_colour_and_opacity_carry_through(self):
        points = [colmap.Point3D(id=1, xyz=(0.0, 0.0, 0.0), rgb=(255, 0, 128),
                                 error=0.0, track_length=2),
                  colmap.Point3D(id=2, xyz=(1.0, 0.0, 0.0), rgb=(0, 255, 0),
                                 error=0.0, track_length=2)]
        splats = sparse.initial_gaussians(points, opacity=0.25)

        self.assertEqual(len(splats), 2)
        self.assertAlmostEqual(splats[0].colour[0], 1.0, places=6)
        self.assertAlmostEqual(splats[0].colour[1], 0.0, places=6)
        self.assertAlmostEqual(splats[1].colour[1], 1.0, places=6)
        self.assertAlmostEqual(splats[0].opacity, 0.25, places=6)
        self.assertEqual(splats[0].rotation, (1.0, 0.0, 0.0, 0.0))

    def test_a_single_point_still_gets_a_size(self):
        points = [colmap.Point3D(id=1, xyz=(0.0, 0.0, 0.0), rgb=(1, 2, 3),
                                 error=0.0, track_length=1)]
        splats = sparse.initial_gaussians(points)
        self.assertEqual(len(splats), 1)
        self.assertGreater(splats[0].scale[0], 0.0)
        self.assertTrue(math.isfinite(splats[0].scale[0]))

    def test_an_empty_cloud_is_empty(self):
        self.assertEqual(sparse.initial_gaussians([]), [])


class CommandLine(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)

    def tearDown(self):
        self._directory.cleanup()

    def test_export_writes_a_readable_file(self):
        model = synthesise.simple_model(self.root / "sparse", point_count=12)
        output = self.root / "points.ply"

        self.assertEqual(cli.main(["export", str(model), str(output)]), 0)
        self.assertTrue(output.is_file())
        self.assertEqual(len(gaussians.read_ply(output)), 12)

    def test_inspect_reads_a_model(self):
        model = synthesise.simple_model(self.root / "sparse", point_count=6)
        self.assertEqual(cli.main(["inspect", str(model)]), 0)

    def test_a_missing_model_fails_without_a_traceback(self):
        self.assertEqual(cli.main(["inspect", str(self.root / "nothing")]), 1)

    def test_training_reports_what_it_needs(self):
        self.assertEqual(cli.main(["train"]), 2)


if __name__ == "__main__":
    unittest.main()
