"""Validate the F6D_D factory package boundary without building/flashing firmware."""

from copy import deepcopy
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "resource_store"))
from package_factory import FIRMWARE_LAYOUT, digest, read_package, validate_firmware_layout


def make_package() -> bytes:
    header = bytearray(52)
    header[:12] = b"111\0\0\0\0\0bpkg"
    struct.pack_into("<I", header, 16, 4)
    header[20:26] = b"SHA256"
    data = header
    for index, (address, capacity) in enumerate(FIRMWARE_LAYOUT):
        payload = f"firmware-{index}".encode("ascii")
        image = bytearray(364)
        name = f"image{index}.bin".encode("ascii")
        image[:len(name)] = name
        struct.pack_into("<4I", image, 64, address, capacity, len(data), len(payload))
        image[80:144] = digest(payload).encode("ascii")
        data += image + payload
    return bytes(data)


class FactoryPackageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="f6dd-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.package = Path(self.temporary.name) / "normal.binpkg"
        self.package.write_bytes(make_package())
        self.images = read_package(self.package)

    def test_accepts_d_pro_layout_and_preserves_internal_fs_boundary(self):
        validate_firmware_layout(self.images)
        app = self.images[3]
        self.assertEqual(app["address"] - 0x800000 + app["capacity"], 0x3BC000)
        self.assertEqual(0x3E4000 - 0x3BC000, 160 * 1024)

    def test_rejects_stale_a_application_layout(self):
        images = deepcopy(self.images)
        images[3].update(address=0xA56000, capacity=0x166000)
        with self.assertRaisesRegex(ValueError, "rebuild"):
            validate_firmware_layout(images)

    def test_rejects_default_d_layout_without_pro_expansion(self):
        images = deepcopy(self.images)
        images[3]["capacity"] = 0xCB000
        with self.assertRaises(ValueError):
            validate_firmware_layout(images)

    def test_rejects_old_expansion_that_overlaps_internal_fs(self):
        images = deepcopy(self.images)
        images[3]["capacity"] = 0x166000
        with self.assertRaises(ValueError):
            validate_firmware_layout(images)

    def test_rejects_wrong_base_layout(self):
        images = deepcopy(self.images)
        images[1]["capacity"] -= 0x10000
        with self.assertRaises(ValueError):
            validate_firmware_layout(images)

    def test_rejects_oversize_or_extra_images(self):
        images = deepcopy(self.images)
        images[3]["size"] = images[3]["capacity"] + 1
        with self.assertRaises(ValueError):
            validate_firmware_layout(images)
        with self.assertRaises(ValueError):
            validate_firmware_layout(self.images + self.images[:1])

    def test_rejects_corrupt_payload_and_trailing_bytes(self):
        corrupted = bytearray(make_package())
        corrupted[-1] ^= 1
        self.package.write_bytes(corrupted)
        with self.assertRaisesRegex(ValueError, "SHA256"):
            read_package(self.package)
        self.package.write_bytes(make_package() + b"trailing")
        with self.assertRaisesRegex(ValueError, "Unexpected bytes"):
            read_package(self.package)


if __name__ == "__main__":
    unittest.main(verbosity=2)
