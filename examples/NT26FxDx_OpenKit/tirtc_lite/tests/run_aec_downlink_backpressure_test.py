#!/usr/bin/env python3
"""Exercise actual WX/DEV/LIVE/GROUP playback, cleanup and uplink functions.

Only queue, clock, capture and playback boundaries are faked. Rejected PCM is
never accepted by the fake HAL, matching the AEC whole-block busy contract.
Both compile switches run; --source-root can verify a saved pre-fix snapshot.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from run_aec_uplink_guard_test import function
from run_dev_playback_gain_test import compiler_command


CONFIGS = (
    ("wechat_call.c", "WX", "wx_audio_frame_t", (
        "wx_audio_slot_release", "wx_audio_queued_decrement", "wx_discard_audio",
        "wx_play_one", "wx_service_downlink", "wx_uplink_is_current_locked", "wx_send_uplink")),
    ("device_call.c", "DEV", "dev_audio_frame_t", (
        "dev_audio_slot_release", "dev_audio_queued_decrement", "dev_drain_audio",
        "dev_play_tail_pending", "dev_play_one", "dev_service_downlink",
        "dev_uplink_job_current_locked", "dev_send_uplink")),
    ("platform_intercom.c", "LIVE", "live_rx_frame_t", (
        "live_rx_slot_release", "live_rx_queued_decrement", "live_note_rx_drop",
        "live_discard_rx_queue", "live_play_one_downlink", "live_service_downlink",
        "live_uplink_is_current_locked", "live_send_uplink_20ms")),
    ("group_intercom.c", "ROOM", "room_audio_slot_t", ("room_drain_pools", "room_audio_tick")),
)


def definitions(source, names):
    result = []
    lines = source.splitlines()
    for name in names:
        index = next(i for i, line in enumerate(lines)
                     if re.match(r"^#define\s+" + name + r"\b", line))
        definition = lines[index]
        while definition.endswith("\\"):
            index += 1
            definition += "\n" + lines[index]
        result.append(definition)
    return "\n".join(result) + "\n"


def typedef(source, name):
    return re.search(r"typedef struct\s*\{[^}]*\}\s*" + name + ";", source).group()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--source-root", type=Path, help="Alternate directory containing features/*.c")
    parser.add_argument("--aec", type=int, choices=(0, 1), help="Only one compile switch")
    parser.add_argument("--kind", type=int, choices=range(4), help="Only WX=0/DEV=1/LIVE=2/GROUP=3")
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    app = Path(__file__).resolve().parents[1]
    sources = args.source_root or app / "src"
    with tempfile.TemporaryDirectory(prefix="lite-aec-backpressure-") as temp:
        out = Path(temp)
        for kind, (filename, prefix, frame_type, names) in enumerate(CONFIGS):
            if args.kind is not None and args.kind != kind:
                continue
            source = (sources / "features" / filename).read_text(encoding="utf-8")
            if kind == 3:
                constants = ("ROOM_RX_SLOTS", "ROOM_RX_MAX", "ROOM_RX_MAX_AGE", "ROOM_SEND_HIGH_WATER",
                             "ROOM_ERR_AUDIO", "ROOM_STREAM")
            elif kind == 2:
                constants = ("LIVE_RX_QUEUE_DEPTH", "LIVE_RX_PAYLOAD_MAX", "LIVE_PLAY_PCM_MAX_SAMPLES",
                             "LIVE_PREBUFFER_PACKETS", "LIVE_PREBUFFER_TIMEOUT_MS",
                             "LIVE_UPLINK_8K_SAMPLES", "LIVE_UPLINK_STREAM_ID")
            else:
                constants = tuple(prefix + suffix for suffix in (
                    "_AUDIO_QUEUE_DEPTH", "_AUDIO_PAYLOAD_MAX", "_PLAY_PCM_MAX_SAMPLES",
                    "_PREBUFFER_TIMEOUT_MS", "_UPLINK_SAMPLES_8K", "_STREAM_ID", "_ERR_AUDIO"))
                constants += (("WX_PREBUFFER_PACKETS",) if kind == 0 else (
                    "DEV_PLAY_BATCH_MAX", "DEV_PLAY_BUDGET_MS", "DEV_PREBUFFER_MS", "DEV_AUDIO_FRAME_BYTES"))
            types = definitions(source, constants) + typedef(source, frame_type) + "\n"
            # Keep extracted senders paired with their real timeout state.
            # Older --source-root snapshots legitimately have none of these.
            capture_state = re.findall(
                r"^static (?:volatile )?(?:bool|uint32_t) "
                r"s_(?:record_failure_(?:active|since|generation)|uplink_epoch);$",
                source, re.M)
            capture_limit = re.findall(r"^#define \w+_RECORD_FAILURE_LIMIT_MS[^\n]*", source, re.M)
            types += "#if DEMO_AI_AUDIO_AEC_ENABLE\n" + "\n".join(capture_limit + capture_state) + "\n#endif\n"
            if kind == 1:
                types += typedef(source, "dev_rx_session_stats_t") + "\n" + typedef(source, "dev_uplink_job_t")
            if kind != 3:
                depth = prefix + ("_RX_QUEUE_DEPTH" if kind == 2 else "_AUDIO_QUEUE_DEPTH")
                types += f"\n#define RX_FRAME {frame_type}\n#define RX_DEPTH {depth}\n"
                # Extract the real pending declaration, without requiring it in
                # old snapshots used for a negative regression demonstration.
                pending = re.search(r"^static int s_play_pending_slot[^;]*;", source, re.M)
                types += "#if DEMO_AI_AUDIO_AEC_ENABLE\n" + (
                    pending.group() if pending else "static int s_play_pending_slot = -1;") + "\n#endif\n"
            funcs = []
            for name in names:
                code = function(source, name)
                if name in ("wx_uplink_is_current_locked", "live_uplink_is_current_locked"):
                    code = "#if DEMO_AI_AUDIO_AEC_ENABLE\n" + code + "#endif\n"
                funcs.append(code)
            (out / "backpressure_types.inc").write_text(types, encoding="utf-8")
            (out / "backpressure_functions.inc").write_text("\n".join(funcs), encoding="utf-8")
            settings = (args.aec,) if args.aec is not None else (0, 1)
            for enabled in settings:
                exe = out / f"backpressure-{kind}-{enabled}.exe"
                subprocess.run(cc + ["-std=c99", "-O2", f"-DTEST_KIND={kind}",
                                     f"-DDEMO_AI_AUDIO_AEC_ENABLE={enabled}",
                                     "-I" + str(out), "-I" + str(app / "include"),
                                     "-I" + str(app / "sdk/include"),
                                     str(app / "tests/aec_downlink_backpressure_test.c"),
                                     str(app / "src/media/g711_codec.c"), "-o", str(exe)], check=True)
                subprocess.run([str(exe)], check=True, timeout=10)
    print("PASS: actual non-AI playback backpressure, ordering, cleanup and uplink")


if __name__ == "__main__":
    main()
