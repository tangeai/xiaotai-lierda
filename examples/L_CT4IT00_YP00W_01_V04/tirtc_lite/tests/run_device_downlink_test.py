#!/usr/bin/env python3
"""Compile production DEV receive functions against bounded queue/clock fakes."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'^static (?:bool|void|int) ' + name +
                      r'\([^;]*?\)\s*\n\{.*?^\}', source, re.M | re.S)
    if not match:
        raise RuntimeError('Missing production function: ' + name)
    return match.group(0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc', help='C compiler executable (also accepts zig)')
    args = parser.parse_args()
    cc = shutil.which(args.cc)
    if not cc:
        parser.error('Compiler not found; pass --cc with its executable path')
    command = [cc] + (['cc'] if Path(cc).stem.lower() == 'zig' else [])
    app = Path(__file__).resolve().parents[1]
    source = (app / 'src/features/device_call.c').read_text(encoding='utf-8')
    names = ('DEV_AUDIO_QUEUE_DEPTH', 'DEV_PLAY_BATCH_MAX', 'DEV_PLAY_BUDGET_MS',
             'DEV_AUDIO_FRAME_BYTES', 'DEV_AUDIO_PAYLOAD_MAX',
             'DEV_PLAY_PCM_MAX_SAMPLES', 'DEV_STREAM_ID', 'DEV_PREBUFFER_MS',
             'DEV_PREBUFFER_TIMEOUT_MS')
    definitions = []
    lines = source.splitlines()
    for name in names:
        index = next((i for i, line in enumerate(lines)
                      if re.match(r'^#define ' + name + r'\b', line)), None)
        if index is None:
            raise RuntimeError('Missing production constant: ' + name)
        definition = lines[index]
        while definition.endswith('\\'):
            index += 1
            definition += '\n' + lines[index]
        definitions.append(definition)
    for name in ('dev_audio_frame_t', 'dev_rx_session_stats_t', 'dev_rx_note_e'):
        match = re.search(r'typedef (?:struct|enum)\s*\{[^}]*\} ' + name + ';', source)
        if not match:
            raise RuntimeError('Missing production type: ' + name)
        definitions.append(match.group(0))
    functions = '\n\n'.join(function(source, name) for name in (
        'dev_audio_slot_acquire', 'dev_audio_producer_done', 'dev_audio_slot_release',
        'dev_audio_queued_decrement', 'dev_reset_downlink', 'dev_reset_rx_session_stats',
        'dev_rx_note', 'dev_snapshot_rx_session', 'dev_play_tail_pending',
        'dev_drain_audio', 'dev_on_audio', 'dev_play_one', 'dev_service_downlink'))
    # These state resets must stay connected to the real lifecycle entry points.
    assert 'dev_reset_downlink();' in function(source, 'dev_start_audio')
    assert 'dev_reset_downlink();' in function(source, 'dev_finish_session')
    assert 'dev_reset_rx_session_stats();' in function(source, 'dev_begin_session')
    with tempfile.TemporaryDirectory(prefix='lite-device-downlink-') as output:
        output = Path(output)
        (output / 'device_downlink_types.inc').write_text('\n'.join(definitions), encoding='utf-8')
        (output / 'device_downlink_functions.inc').write_text(functions, encoding='utf-8')
        exe = output / 'device_downlink_test.exe'
        subprocess.run(command + ['-std=c99', '-Wall', '-Wextra', '-Werror',
                                  '-I' + str(output), '-I' + str(app / 'include'),
                                  '-I' + str(app / 'sdk/include'),
                                  str(app / 'tests/device_downlink_test.c'),
                                  str(app / 'src/media/g711_codec.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
