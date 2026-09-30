#!/usr/bin/env python3
"""Compile the production Lite audio adapter against local SDK/RTOS fakes.

Examples: --cc gcc, --cc "zig cc", or --cc /path/to/zig.exe.
No board, serial port, Pro test files, or generated production sources are used.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def compiler_command(value, parser):
    executable = shutil.which(value)
    if executable:
        command = [executable]
    else:
        command = shlex.split(value, posix=os.name != "nt")
        command = [part.strip('"') for part in command]
        if not command or not shutil.which(command[0]):
            parser.error("C compiler not found; pass --cc with its executable or command")
    if Path(command[0]).stem.lower() == "zig" and len(command) == 1:
        command.append("cc")
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"),
                        help="C compiler executable or command, including 'zig cc'")
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    app = Path(__file__).resolve().parents[1]
    root = app.parents[2]
    with tempfile.TemporaryDirectory(prefix="lite-dev-playback-gain-") as output:
        for group in (False, True):
            exe = Path(output) / ("gain-group.exe" if group else "gain-basic.exe")
            command = cc + ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror"]
            if group:
                command.append("-DHWDEMO_GROUP_ROOM_EN")
            command += [
                "-I", str(app / "tests/dev_playback_gain_host_stubs"),
                "-I", str(app / "include"),
                "-I", str(root / "components/kernel/lierda_api/liot_audio2"),
                str(app / "tests/dev_playback_gain_test.c"),
                str(app / "src/media/audio_device.c"),
                "-o", str(exe), "-lm",
            ]
            subprocess.run(command, check=True)
            subprocess.run([str(exe)], check=True)
    print("PASS: production Lite gain tests with Group enabled and disabled")


if __name__ == "__main__":
    main()
