import tempfile
import unittest
from pathlib import Path

from r4drecon import colmap

from . import synthesise


class ReadModel(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory()
        self.root = Path(self._directory.name)

    def tearDown(self):
        self._directory.cleanup()

    def test_reads_what_was_written(self):
        synthesise.simple_model(self.root, point_count=5)
        model = colmap.read_model(self.root)

        self.assertEqual(len(model.cameras), 1)
        self.assertEqual(len(model.images), 2)
        self.assertEqual(len(model.points), 5)

        camera = model.cameras[1]
        self.assertEqual(camera.model, "PINHOLE")
        self.assertEqual((camera.width, camera.height), (640, 480))
        self.assertEqual(camera.focal, (500.0, 505.0))

        image = model.images[2]
        self.assertEqual(image.name, "frame_0002.jpg")
        self.assertEqual(image.translation, (1.0, 2.0, 3.0))
        self.assertEqual(image.camera_id, 1)

        point = model.points[0]
        self.assertEqual(point.id, 1)
        self.assertEqual(point.rgb, (0, 0, 0))
        self.assertEqual(point.track_length, 2)

    def test_variable_length_records_do_not_desynchronise(self):
        # Names and tracks have no fixed size, so a reader that mis-advances
        # would still produce the first record correctly and garbage after it.
        cameras = [(1, 0, 100, 100, (50.0, 50.0, 50.0))]
        images = [
            (1, (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 1, "a.jpg",
             [(1.0, 2.0, 1)]),
            (2, (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 1, "a_much_longer_name.png",
             [(1.0, 2.0, 1), (3.0, 4.0, 2), (5.0, 6.0, 3)]),
            (3, (1.0, 0.0, 0.0, 0.0), (9.0, 8.0, 7.0), 1, "z.jpg", []),
        ]
        points = [
            (1, (0.0, 0.0, 0.0), (1, 2, 3), 0.1, [(1, 0)]),
            (2, (1.0, 1.0, 1.0), (4, 5, 6), 0.2, [(1, 0), (2, 1), (3, 2)]),
            (3, (2.0, 2.0, 2.0), (7, 8, 9), 0.3, []),
        ]
        synthesise.write_model(self.root, cameras, images, points)

        model = colmap.read_model(self.root)
        self.assertEqual(model.images[3].name, "z.jpg")
        self.assertEqual(model.images[3].translation, (9.0, 8.0, 7.0))
        self.assertEqual(model.points[2].rgb, (7, 8, 9))
        self.assertEqual(model.points[2].track_length, 0)

    def test_simple_pinhole_shares_one_focal_length(self):
        synthesise.write_cameras(self.root / "cameras.bin",
                                 [(7, 0, 320, 240, (123.0, 160.0, 120.0))])
        cameras = colmap.read_cameras(self.root / "cameras.bin")
        self.assertEqual(cameras[7].model, "SIMPLE_PINHOLE")
        self.assertEqual(cameras[7].focal, (123.0, 123.0))

    def test_rejects_a_missing_model(self):
        with self.assertRaises(colmap.ColmapError) as caught:
            colmap.read_model(self.root)
        self.assertIn("cameras.bin", str(caught.exception))

    def test_rejects_an_unknown_camera_model(self):
        synthesise.write_cameras(self.root / "cameras.bin",
                                 [(1, 99, 10, 10, ())])
        with self.assertRaises(colmap.ColmapError) as caught:
            colmap.read_cameras(self.root / "cameras.bin")
        self.assertIn("99", str(caught.exception))

    def test_rejects_a_truncated_file(self):
        synthesise.simple_model(self.root, point_count=4)
        path = self.root / "points3D.bin"
        data = path.read_bytes()
        path.write_bytes(data[:len(data) // 2])

        with self.assertRaises(colmap.ColmapError) as caught:
            colmap.read_points(path)
        self.assertIn("ran out of data", str(caught.exception))

    def test_rejects_an_unterminated_name(self):
        path = self.root / "images.bin"
        synthesise.write_images(path, [
            (1, (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 1, "a.jpg", [])])
        data = path.read_bytes()
        path.write_bytes(data.replace(b"a.jpg\x00", b"a.jpg"))

        with self.assertRaises(colmap.ColmapError):
            colmap.read_images(path)


if __name__ == "__main__":
    unittest.main()
