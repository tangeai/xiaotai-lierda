#!/usr/bin/env python3
"""Exercise production Lite AEC stream adaptation with deterministic SDK fakes.

Each scenario uses a fresh process, so no test-only reset enters production.
This checks adapter ownership/data flow, not acoustic cancellation quality or
the closed vendor driver's synchronization on an actual NT26F6D0.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from run_dev_playback_gain_test import compiler_command


SCENARIOS = (
    "capture", "capture-ready-contention", "timeout", "overflow", "stop-owner", "stop-wait",
    "record-only-rebuild", "duplex-rebuild", "rebuild-failure", "rebuild-timeout", "permissions",
    "playback", "short-packet-continuity", "continuous-short-packets", "uneven-packet-boundary", "short-tail",
    "stop-play-contention", "stop-lease-reuse", "release-tail", "playback-pacing", "gain", "backpressure", "sdk-failure", "init-failure", "aec-failure",
    "stream-failure", "worker-failure", "startup-timeout", "mutex-failure", "levels-contention", "worker-timeout",
    "slow-vendor-stop", "slow-worker-completion", "drain-grace", "drain-grace-blocked",
    "record-stop-failure", "long-idle", "clock-wrap",
    "volume-reset-1", "volume-reset-40", "volume-reset-120", "volume-read-retry", "volume-stop-owner",
    "volume-stale-service", "volume-service-contention",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--scenario", choices=SCENARIOS, action="append",
                        help="run only selected scenarios; repeat for several")
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    app = Path(__file__).resolve().parents[1]
    root = app.parents[2]
    with tempfile.TemporaryDirectory(prefix="lite-aec-") as output:
        for group in (False, True):
            exe = Path(output) / ("aec-group.exe" if group else "aec-basic.exe")
            command = cc + ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-DDEMO_AI_AUDIO_AEC_ENABLE=1", "-DDEMO_AI_AUDIO_AEC_TEST"]
            if group:
                command.append("-DHWDEMO_GROUP_ROOM_EN")
            command += [
                "-I", str(app / "tests/audio_aec_host_stubs"),
                "-I", str(app / "tests/dev_playback_gain_host_stubs"),
                "-I", str(app / "include"),
                "-I", str(root / "components/kernel/lierda_api/liot_audio2"),
                str(app / "tests/audio_aec_test.c"),
                str(app / "src/media/audio_device.c"),
                "-o", str(exe), "-lm",
            ]
            subprocess.run(command, check=True)
            for scenario in args.scenario or SCENARIOS:
                subprocess.run([str(exe), scenario], check=True, timeout=10)
    print("PASS: production Lite AEC adapter scenarios with Group enabled and disabled")


if __name__ == "__main__":
    main()
