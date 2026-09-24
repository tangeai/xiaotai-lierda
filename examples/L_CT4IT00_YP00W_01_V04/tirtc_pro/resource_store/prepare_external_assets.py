"""Extract the unchanged UI bitmaps for the separate external-Flash installer.

The original source arrays remain as host-test fixtures behind the production
external-resource switch; this never quantizes a glyph or changes a pixel.
"""
from pathlib import Path
import hashlib
import json
import re
import struct
import zlib

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parents[2]


def array(text, name, width):
    match = re.search(r"\b" + name + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not match:
        raise ValueError("Missing original bitmap array " + name)
    values = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", match[1])]
    return bytes(values) if width == 1 else struct.pack("<" + "H" * len(values), *values)


def main():
    font = array((APP / "ui/font/tirtc_font_14.c").read_text(encoding="utf-8"), "glyph_bitmap", 1)
    background = array((APP / "ui/assets/ui_face_background.c").read_text(encoding="utf-8"), "ui_face_background_pixels", 2)
    report_font = json.loads((APP / "ui/font/generation_report.json").read_text(encoding="utf-8"))
    assert len(font) == 631201 and len(background) == 320 * 240 * 2
    assert hashlib.sha256(font).hexdigest() == "fbfbc213d8457d56d200937f56bf9b94b0856103de90f45fb2abf71cabe7e398"
    target = APP / "resource_store"
    target.mkdir(parents=True, exist_ok=True)
    header = "#ifndef TIRTC_ASSET_MANIFEST_H\n#define TIRTC_ASSET_MANIFEST_H\n#include <stdint.h>\n"
    source = '/* Original 4-bpp glyphs and RGB565 background. Installer build only. */\n#include "asset_manifest.h"\n'
    report = {"format": "unaltered bytes; no compression or quantization", "files": []}
    output = ROOT / "release/tirtc_pro/assets14"
    output.mkdir(parents=True, exist_ok=True)
    for key, name, value in [("FONT", "font", font), ("BACKGROUND", "background", background)]:
        header += f"#define TIRTC_{key}_BYTES {len(value)}U\n#define TIRTC_{key}_CRC32 0x{zlib.crc32(value):08x}U\n"
        header += f"extern const uint8_t tirtc_install_{name}[TIRTC_{key}_BYTES];\n"
        source += f"const uint8_t tirtc_install_{name}[TIRTC_{key}_BYTES] = {{\n"
        source += "\n".join("    " + ",".join(f"0x{x:02x}" for x in value[i:i+24]) + "," for i in range(0, len(value), 24))
        source += "\n};\n"
        filename = "ui14-font.bin" if key == "FONT" else "ui13-bg.bin"
        (output / filename).write_bytes(value)
        report["files"].append({"name": filename, "size": len(value), "crc32": f"{zlib.crc32(value):08x}", "sha256": hashlib.sha256(value).hexdigest()})
    (target / "asset_manifest.h").write_text(header + "#endif\n", encoding="utf-8")
    (target / "installer_blob.c").write_text(source, encoding="utf-8")
    (output / "manifest.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
