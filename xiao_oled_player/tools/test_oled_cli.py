"""Converter regression checks. Run with python -m unittest discover -s tools."""
import contextlib
import io
from pathlib import Path
import struct
import tempfile
import unittest

import cv2
import lz4.block
import numpy as np

import oled_cli


class ConverterTests(unittest.TestCase):
    def test_resize_modes_preserve_expected_content(self):
        # A landscape image with red edges and a green square in the center.
        image = np.full((128, 256, 3), (0, 0, 255), dtype=np.uint8)
        image[:, 64:192] = (0, 255, 0)
        filled = oled_cli.resize_frame(image)
        self.assertEqual(filled.shape, (128, 128, 3))
        np.testing.assert_array_equal(filled, image[:, 64:192])
        fitted = oled_cli.resize_frame(image, "fit")
        self.assertEqual(fitted.shape, (128, 128, 3))
        self.assertFalse(fitted[:32].any())
        self.assertFalse(fitted[96:].any())
        np.testing.assert_array_equal(fitted[64, 0], (0, 0, 255))
        stretched = oled_cli.resize_frame(image, "stretch")
        self.assertEqual(stretched.shape, (128, 128, 3))
        np.testing.assert_array_equal(stretched[0, 0], (0, 0, 255))
        np.testing.assert_array_equal(stretched[64, 64], (0, 255, 0))

    def test_streams_decode_and_preserve_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.avi"
            writer = cv2.VideoWriter(str(source), cv2.VideoWriter_fourcc(*"FFV1"),
                                     30, (128, 128))
            self.assertTrue(writer.isOpened(), "FFV1 encoder required")
            try:
                for index in range(30):
                    frame = np.full((128, 128, 3), (0, 0, 255), dtype=np.uint8)
                    frame[:, index:index + 8] = (0, 255, 0)
                    writer.write(frame)
            finally:
                writer.release()
            for codec, fps in (("lz4", 12), ("mjpeg", 12), ("mjpeg", 60)):
                with self.subTest(codec=codec, fps=fps):
                    output = Path(directory) / (codec + str(fps))
                    with contextlib.redirect_stdout(io.StringIO()):
                        oled_cli.encode_video(str(source), str(output), fps, codec=codec)
                    data = output.read_bytes()
                    magic = b"LZ4S" if codec == "lz4" else b"MJPS"
                    self.assertEqual(struct.unpack_from("<4sHHHH", data),
                                     (magic, 128, 128, fps, 0))
                    offset, count = 12, 0
                    while True:
                        size, = struct.unpack_from("<H", data, offset)
                        offset += 2
                        if not size:
                            break
                        self.assertLessEqual(size, 33000)
                        block = data[offset:offset + size]
                        self.assertEqual(len(block), size)
                        offset += size
                        if codec == "lz4":
                            raw = block if size == 32768 else lz4.block.decompress(
                                block, uncompressed_size=32768)
                            self.assertEqual(len(raw), 32768)
                            # The final pixel is red, encoded big-endian RGB565.
                            self.assertEqual(raw[-2:], b"\xf8\x00")
                        else:
                            self.assertEqual(block[:2], b"\xff\xd8")
                            self.assertEqual(block[-2:], b"\xff\xd9")
                            decoded = cv2.imdecode(np.frombuffer(block, np.uint8),
                                                   cv2.IMREAD_COLOR)
                            self.assertEqual(decoded.shape, (128, 128, 3))
                            self.assertGreater(int(decoded[-1, -1, 2]), 240)
                        count += 1
                    self.assertEqual(offset, len(data))
                    self.assertEqual(count, fps)  # One second at either output FPS.

    def test_invalid_settings_fail_before_opening_output(self):
        for settings in ({"fps": 0}, {"fps": 61}, {"quality": 0}, {"quality": 101},
                         {"codec": "invalid"}):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                oled_cli.encode_video("unused", "unused", **settings)


if __name__ == "__main__":
    unittest.main()
