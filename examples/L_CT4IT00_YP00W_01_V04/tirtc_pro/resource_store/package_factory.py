"""Package the unchanged pro firmware plus its SPI1 factory filesystem.

Run after build_tirtc_pro.bat. This Windows host tool never flashes hardware.
The published package overwrites the external 4 MiB filesystem when flashed.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

# Keep host build artifacts out of the application source directory.
sys.dont_write_bytecode = True
from build_factory_image import DEFAULT_REPO, IMAGE_SIZE, build_image


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_package(path: Path) -> list[dict]:
    """Check the bundled fcelf format, every payload hash and exact EOF.

    Only this SDK's unsigned SHA256 binpkg layout is accepted. No payload is
    executed or extracted using names supplied by the package.
    """
    data = path.read_bytes()
    if len(data) < 52 or data[:12] != b"111\0\0\0\0\0bpkg" or data[20:26] != b"SHA256":
        raise ValueError(f"Unsupported or incomplete package: {path}")
    count = struct.unpack_from("<I", data, 16)[0]
    offset = 52 + struct.unpack_from("<H", data, 30)[0]
    if not 1 <= count <= 16:
        raise ValueError("Unexpected package image count")
    images = []
    for _ in range(count):
        if offset + 364 > len(data):
            raise ValueError("Truncated package image header")
        address, capacity, position, length = struct.unpack_from("<4I", data, offset + 64)
        if position != offset or length > capacity or length == 0:
            raise ValueError("Invalid package image bounds")
        end = offset + 364 + length
        payload = data[offset + 364:end]
        expected_hash = data[offset + 80:offset + 144].decode("ascii").lower()
        if end > len(data) or digest(payload) != expected_hash:
            raise ValueError("Package image SHA256 mismatch")
        images.append({
            "name": data[offset:offset + 64].split(b"\0", 1)[0].decode("ascii"),
            "type": data[offset + 336:offset + 364].split(b"\0", 1)[0].decode("ascii"),
            "address": address, "capacity": capacity,
            "size": length, "sha256": expected_hash,
        })
        offset = end
    if offset != len(data):
        raise ValueError("Unexpected bytes after final package image")
    return images


def main() -> None:
    repo = DEFAULT_REPO
    app = Path(__file__).resolve().parent.parent
    build = repo / "gccout/tirtc_pro"
    output = repo / "gccout/tirtc_pro_factory"
    release = repo / "firmware/pro"
    match = re.search(r"^APP_VERSION\s*:?=\s*(\w+)\s*$", (app / "config").read_text(), re.M)
    if not match:
        raise ValueError("Missing pro APP_VERSION")
    normal_package = build / f"TiRTC_pro_NT26F6D0_{match.group(1)}.binpkg"
    normal_images = read_package(normal_package)
    if len(normal_images) != 4:
        raise ValueError("Build the normal four-image pro package first")
    base = app / "board_compat/basePkg/F6D_A"
    product_match = re.search(r"^#define\s+PKG_PRODUCT\s+(\w+)\s*$",
                              (base / "mem_map.txt").read_text(), re.M)
    if not product_match or product_match.group(1) != "EC718PM_PRD":
        raise ValueError("Unexpected base package for this pro board")
    sources = [
        (base / "ap_bootloader.bin", "BL_PKGIMG_LNA", "BOOTLOADER_PKGIMG_LIMIT_SIZE"),
        (base / "ap_lierda_app.bin", "AP_PKGIMG_LNA", "AP_PKGIMG_LIMIT_SIZE"),
        (base / "cp-demo-flash.bin", "CP_PKGIMG_LNA", "CP_PKGIMG_LIMIT_SIZE"),
        (build / "TiRTC_pro_NT26F6D0.bin", "PKGFLXAPP_APP0_LNA", "PKGFLXAPP_APP0_SIZE"),
    ]
    for item, (source, _, _) in zip(normal_images, sources):
        if digest(source.read_bytes()) != item["sha256"]:
            raise ValueError(f"Input differs from normal firmware package: {source}")

    output.mkdir(parents=True, exist_ok=True)
    image_path = output / "ext_lfs.bin"
    filesystem = build_image(repo, image_path)
    memory_map = (build / "mem_map.txt").read_text()
    if re.search(r"^#define\s+EFID01_IMG_LFS_", memory_map, re.M):
        raise ValueError("Normal pro build already defines a SPI1 filesystem image")
    # Packaging-only map: no application CFLAGS, linker layout or SDK changes.
    memory_map += "\n#define EFID01_IMG_LFS_LNA (0x80000000)\n#define EFID01_IMG_LFS_SIZE (0x400000)\n"
    sources.append((image_path, "EFID01_IMG_LFS_LNA", "EFID01_IMG_LFS_SIZE"))
    with tempfile.TemporaryDirectory(prefix=".package-", dir=output) as temporary:
        scratch = Path(temporary)
        definition = scratch / "mem_map.txt"
        definition.write_text(memory_map, encoding="ascii")
        package = scratch / "latest.binpkg"
        command = [str(repo / "tools/fcelf/fcelf.exe"), "-M"]
        for source, address, capacity in sources:
            command += ["-input", str(source), "-addrname", address, "-flashsize", capacity]
        command += ["-pkgmode", "1", "-banoldtool", "1", "-productname", product_match.group(1),
                    "-def", str(definition), "-outfile", str(package)]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=120)
        (output / "package.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise ValueError("fcelf packaging failed; see gccout/tirtc_pro_factory/package.log")
        images = read_package(package)
        if len(images) != 5 or images[:4] != normal_images:
            raise ValueError("Factory package changed the normal firmware images")
        external = images[4]
        if (external["address"] != 0x80000000 or external["capacity"] != IMAGE_SIZE
                or external["size"] != IMAGE_SIZE or external["sha256"] != filesystem["sha256"]):
            raise ValueError("Factory package external filesystem mismatch")
        # The tool's port-specific image type is checked below after packaging.
        if external["type"] != "EFID01":
            raise ValueError(f"Expected SPI1 external image, got {external['type']!r}")
        package_bytes = package.read_bytes()
        report = {
            "product": "pro", "purpose": "factory initialization",
            "external_flash_files_overwritten": True,
            "package": "latest.binpkg", "size": len(package_bytes), "sha256": digest(package_bytes),
            "normal_firmware_payloads_unchanged": True, "images": images,
            "external_filesystem": {key: value for key, value in filesystem.items() if key != "path"},
            "hardware_flash_verified": False,
        }
        # Publish only after the complete package and resource readback pass.
        release.mkdir(parents=True, exist_ok=True)
        staged = release / "latest.binpkg.tmp"
        staged.write_bytes(package_bytes)
        staged.replace(release / "latest.binpkg")
        (release / "SHA256SUMS.txt").write_text(f"{report['sha256']}  latest.binpkg\n", encoding="ascii")
        (release / "manifest.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (output / "mem_map.txt").write_text(memory_map, encoding="ascii")
    print("Ready: firmware/pro/latest.binpkg (firmware + font + background)")
    print("Factory initialization: flashing this package overwrites external Flash files.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise SystemExit(f"Factory packaging failed: {error}")
