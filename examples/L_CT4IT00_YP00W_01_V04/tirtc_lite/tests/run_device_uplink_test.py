"""Fault-inject the actual Lite uplink functions with a bounded RTOS queue fake."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

APP = Path(__file__).resolve().parents[1]
SOURCE = APP / 'src/features/device_call.c'
SDK = APP / 'sdk/include'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc', help='C compiler executable (also accepts zig)')
args = parser.parse_args()
compiler = shutil.which(args.cc)
if not compiler:
    parser.error('Compiler not found; pass --cc with its executable path')
command = [compiler] + (['cc'] if Path(compiler).stem.lower() == 'zig' else [])


def function(source, name):
    match = re.search(r'^static (?:bool|void|int) ' + name + r'\([^;]*?\)\s*\n\{.*?^\}', source, re.M | re.S)
    if not match:
        raise RuntimeError('Missing actual function: ' + name)
    return match.group(0)


source = SOURCE.read_text(encoding='utf-8')
job = re.search(r'typedef struct\s*\{\s*uint32_t generation;\s*uint32_t tirtc_generation;.*?\} dev_uplink_job_t;', source, re.S).group(0)
depth = re.search(r'^#define DEV_UPLINK_QUEUE_DEPTH\s+\d+U', source, re.M).group(0)
max_age = re.search(r'^#define DEV_UPLINK_MAX_AGE_MS\s+\d+U', source, re.M).group(0)
functions = '\n\n'.join(function(source, name) for name in (
    'dev_signal', 'dev_uplink_reset_session_stats', 'dev_uplink_job_current_locked', 'dev_drain_uplink',
    'dev_send_uplink_job', 'dev_send_uplink'))

harness = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tirtc/tiRTC.h"
#define DEV_UPLINK_SAMPLES_8K 160U
#define DEV_STREAM_ID 10U
#define DEV_ERR_AUDIO (-5008)
#define TIRTC_LITE_DEV_TIMING 0
#define DEMO_DEV_CHAT_IN_CALL 7
#define DEMO_TIRTC_OWNER_DEV_CHAT 3
#define LIOT_OSI_SUCCESS 0
#define LIOT_NO_WAIT 0
typedef uint8_t uint8;
''' + depth + '\n' + max_age + '\n' + job + r'''
static unsigned s_state, s_generation, s_tirtc_generation;
static bool s_tirtc_claimed, s_tirtc_release_pending, s_request_hangup;
static bool s_request_conn_error;
static int s_request_conn_error_code;
static unsigned s_tx_frames, s_tx_dropped;
static unsigned s_tx_session_queue_full, s_tx_session_sdk_busy, s_tx_session_expired;
static void dev_uplink_reset_session_stats(void);
static void *s_uplink_queue = (void *)1, *s_event_sem = (void *)2;
static int s_audio_lease;
static int16_t s_capture_pcm[320];
static uint8_t s_uplink_alaw[160];
static dev_uplink_job_t queue[DEV_UPLINK_QUEUE_DEPTH];
static unsigned queue_count, capture_sequence, signal_count, sdk_calls, tick;
static int sdk_result, record_error, critical_depth;
static unsigned sdk_advance_ms;
static bool new_call_during_send, new_call_during_enqueue, hangup_during_record;
static dev_uplink_job_t sent;
static void liot_rtos_enter_critical(void) { ++critical_depth; }
static void liot_rtos_exit_critical(void) { assert(critical_depth > 0); --critical_depth; }
static uint32_t liot_rtos_get_running_time(void) { return tick; }
static void liot_trace(const char *format, ...) { (void)format; }
static int liot_rtos_semaphore_release(void *sem) {
    assert(sem == s_event_sem && critical_depth == 0); ++signal_count; return 0;
}
static int liot_rtos_queue_release(void *q, unsigned size, uint8 *bytes, unsigned timeout) {
    assert(q == s_uplink_queue && size == sizeof(dev_uplink_job_t));
    assert(timeout == LIOT_NO_WAIT && critical_depth == 0);
    if (new_call_during_enqueue) {
        ++s_generation; ++s_tirtc_generation; dev_uplink_reset_session_stats();
        return -1;
    }
    if (queue_count == DEV_UPLINK_QUEUE_DEPTH) return -1;
    memcpy(&queue[queue_count++], bytes, size); return LIOT_OSI_SUCCESS;
}
static int liot_rtos_queue_wait(void *q, uint8 *bytes, unsigned size, unsigned timeout) {
    assert(q == s_uplink_queue && size == sizeof(dev_uplink_job_t));
    assert(timeout == LIOT_NO_WAIT && critical_depth == 0);
    if (!queue_count) return -1;
    memcpy(bytes, &queue[0], size);
    --queue_count; memmove(queue, queue + 1, queue_count * sizeof(queue[0]));
    return LIOT_OSI_SUCCESS;
}
static int demo_ai_audio_record_20ms(int *lease, int16_t *pcm) {
    assert(lease == &s_audio_lease && critical_depth == 0);
    ++capture_sequence; tick += 20;
    for (unsigned i = 0; i < 320; ++i) pcm[i] = (int16_t)capture_sequence;
    if (hangup_during_record) s_request_hangup = true;
    return record_error;
}
/* Encoder tags capture ownership here; DSP correctness has separate tests. */
static uint8_t demo_g711_alaw_encode_sample(int16_t sample) { return (uint8_t)sample; }
static int demo_tirtc_send_audio(int owner, unsigned generation,
                                const TIRTCFRAMEINFO *frame, const void *payload) {
    assert(owner == DEMO_TIRTC_OWNER_DEV_CHAT && critical_depth == 0);
    assert(generation == s_tirtc_generation);
    ++sdk_calls; sent.frame = *frame; memcpy(sent.payload, payload, 160);
    tick += sdk_advance_ms;
    if (new_call_during_send) {
        ++s_generation; ++s_tirtc_generation;
        s_tx_frames = s_tx_dropped = 0;
        dev_uplink_reset_session_stats();
        s_request_conn_error = false; s_request_conn_error_code = 0;
    }
    return sdk_result;
}
''' + functions + r'''
static void reset(void) {
    assert(critical_depth == 0);
    s_state = DEMO_DEV_CHAT_IN_CALL; s_generation = 10; s_tirtc_generation = 20;
    s_tirtc_claimed = true;
    s_tirtc_release_pending = s_request_hangup = s_request_conn_error = false;
    s_request_conn_error_code = 0;
    queue_count = capture_sequence = signal_count = sdk_calls = tick = 0;
    s_tx_frames = s_tx_dropped = 0; sdk_result = record_error = 0; sdk_advance_ms = 0;
    dev_uplink_reset_session_stats();
    new_call_during_send = new_call_during_enqueue = hangup_during_record = false;
}
static dev_uplink_job_t pop(void) {
    dev_uplink_job_t job;
    assert(liot_rtos_queue_wait(s_uplink_queue, (uint8 *)&job, sizeof(job), 0) == 0);
    return job;
}
int main(void) {
    dev_uplink_job_t job;
    reset();
    for (unsigned i = 0; i < DEV_UPLINK_QUEUE_DEPTH; ++i) assert(dev_send_uplink() == 0);
    assert(sdk_calls == 0 && queue_count == DEV_UPLINK_QUEUE_DEPTH);
    assert(dev_send_uplink() == 0 && s_tx_dropped == 1 && queue_count == DEV_UPLINK_QUEUE_DEPTH);
    assert(s_tx_session_queue_full == 1 && s_tx_session_sdk_busy == 0);
    for (unsigned i = 0; i < DEV_UPLINK_QUEUE_DEPTH; ++i) {
        job = pop();
        assert(job.generation == 10 && job.tirtc_generation == 20);
        assert(job.frame.stream_id == 10 && job.frame.media == TIRTC_AUDIO_ALAW);
        assert(job.frame.flags == TIRTC_AUDIOSAMPLE_8K16B1C && job.frame.length == 160);
        assert(job.frame.ts == 20 * (i + 1));
        for (unsigned k = 0; k < 160; ++k) assert(job.payload[k] == i + 1);
        dev_send_uplink_job(&job);
        assert(sent.payload[0] == i + 1);
    }
    assert(sdk_calls == DEV_UPLINK_QUEUE_DEPTH && s_tx_frames == DEV_UPLINK_QUEUE_DEPTH);
    puts("PASS: bounded nonblocking queue, owned payloads, ordering, timestamps and format");

    reset();
    for (unsigned i = 0; i < 6; ++i) assert(dev_send_uplink() == 0);
    assert(queue_count == 6 && s_tx_dropped == 0 && sdk_calls == 0);
    while (queue_count) { job = pop(); dev_send_uplink_job(&job); }
    assert(s_tx_frames == 6 && s_tx_session_queue_full == 0);
    puts("PASS: six-frame scheduling burst retained without blocking capture");

    reset(); assert(dev_send_uplink() == 0); job = pop(); ++s_generation;
    dev_send_uplink_job(&job); assert(sdk_calls == 0 && s_tx_frames == 0);
    reset(); assert(dev_send_uplink() == 0); job = pop(); ++s_tirtc_generation;
    dev_send_uplink_job(&job); assert(sdk_calls == 0);
    reset(); assert(dev_send_uplink() == 0); job = pop(); s_tirtc_release_pending = true;
    dev_send_uplink_job(&job); assert(sdk_calls == 0);
    reset(); assert(dev_send_uplink() == 0); job = pop(); s_state = 8;
    dev_send_uplink_job(&job); assert(sdk_calls == 0);
    puts("PASS: stale business/SDK generations and stopping session rejected before SDK");

    for (unsigned mode = 0; mode < 3; ++mode) {
        reset(); assert(dev_send_uplink() == 0); job = pop();
        sdk_result = mode == 0 ? -77 : mode == 1 ? TIRTC_E_BUSY : 0;
        new_call_during_send = true;
        dev_send_uplink_job(&job);
        assert(sdk_calls == 1 && !s_request_conn_error && signal_count == 0);
        assert(s_tx_frames == 0 && s_tx_dropped == 0);
        assert(s_tx_session_queue_full == 0 && s_tx_session_sdk_busy == 0);
        assert(s_tx_session_expired == 0);
    }
    reset(); assert(dev_send_uplink() == 0); job = pop(); sdk_result = -77;
    dev_send_uplink_job(&job);
    assert(s_request_conn_error && s_request_conn_error_code == -77 && signal_count == 1);
    reset(); assert(dev_send_uplink() == 0); job = pop(); sdk_result = TIRTC_E_BUSY;
    dev_send_uplink_job(&job);
    assert(!s_request_conn_error && signal_count == 0 && s_tx_dropped == 1);
    assert(s_tx_session_queue_full == 0 && s_tx_session_sdk_busy == 1);
    puts("PASS: late errors/busy/success cannot alter a newer call; current errors wake owner");

    reset();
    for (unsigned i = 0; i <= DEV_UPLINK_QUEUE_DEPTH; ++i) assert(dev_send_uplink() == 0);
    job = pop(); sdk_result = TIRTC_E_BUSY; dev_send_uplink_job(&job);
    assert(s_tx_dropped == 2 && s_tx_session_queue_full == 1 && s_tx_session_sdk_busy == 1);
    dev_drain_uplink(); ++s_generation; ++s_tirtc_generation;
    dev_uplink_reset_session_stats();
    assert(s_tx_dropped == 2 && s_tx_session_queue_full == 0 && s_tx_session_sdk_busy == 0);
    dev_send_uplink_job(&job);
    assert(s_tx_dropped == 2 && s_tx_session_sdk_busy == 0);
    assert(dev_send_uplink() == 0); job = pop(); dev_send_uplink_job(&job);
    assert(s_tx_dropped == 3 && s_tx_session_queue_full == 0 && s_tx_session_sdk_busy == 1);
    reset(); new_call_during_enqueue = true; assert(dev_send_uplink() == 0);
    assert(s_tx_dropped == 0 && s_tx_session_queue_full == 0 && s_tx_session_sdk_busy == 0);
    puts("PASS: per-session queue/SDK drop reasons reset; cumulative totals and late-call isolation preserved");

    reset(); for (unsigned i = 0; i < DEV_UPLINK_QUEUE_DEPTH; ++i) assert(dev_send_uplink() == 0);
    dev_drain_uplink(); assert(queue_count == 0 && sdk_calls == 0);
    ++s_generation; ++s_tirtc_generation;
    assert(dev_send_uplink() == 0); job = pop(); dev_send_uplink_job(&job);
    assert(sdk_calls == 1 && sent.payload[0] == DEV_UPLINK_QUEUE_DEPTH + 1);
    reset(); hangup_during_record = true; assert(dev_send_uplink() == 0);
    assert(queue_count == 0 && sdk_calls == 0);
    reset(); record_error = -1; assert(dev_send_uplink() == DEV_ERR_AUDIO && queue_count == 0);
    puts("PASS: drain/restart, cancellation during capture, record failure");

    assert(DEV_UPLINK_MAX_AGE_MS == 300U);
    for (unsigned wrapping = 0; wrapping < 2; ++wrapping) {
        for (unsigned age = 299U; age <= 301U; ++age) {
            reset(); tick = wrapping ? UINT32_MAX - 39U : 0U;
            assert(dev_send_uplink() == 0); job = pop();
            tick = job.frame.ts + age;
            if (wrapping) assert(tick < job.frame.ts);
            dev_send_uplink_job(&job);
            bool expired = age > DEV_UPLINK_MAX_AGE_MS;
            assert(sdk_calls == (expired ? 0U : 1U));
            assert(s_tx_frames == (expired ? 0U : 1U));
            assert(s_tx_dropped == (expired ? 1U : 0U));
            assert(s_tx_session_expired == (expired ? 1U : 0U));
            assert(!s_tx_session_queue_full && !s_tx_session_sdk_busy);
            assert(!s_request_conn_error && !signal_count);
        }
    }
    puts("PASS: 299/300ms admitted, 301ms expired, including uint32 clock wrap");

    reset();
    for (unsigned i = 0; i < DEV_UPLINK_QUEUE_DEPTH; ++i) assert(dev_send_uplink() == 0);
    tick = 371U; /* First three timestamps20/40/60 expired;80..160 remain fresh. */
    for (unsigned i = 0; i < DEV_UPLINK_QUEUE_DEPTH; ++i) {
        job = pop(); unsigned before = sdk_calls;
        dev_send_uplink_job(&job);
        if (i < 3U) assert(sdk_calls == before);
        else {
            assert(sdk_calls == before + 1U);
            assert(sent.payload[0] == i + 1U && sent.frame.ts == 20U * (i + 1U));
        }
    }
    assert(s_tx_session_expired == 3U && s_tx_dropped == 3U && s_tx_frames == 5U);
    assert(!queue_count && !s_tx_session_queue_full && !s_tx_session_sdk_busy);
    puts("PASS: expired prefix discarded while remaining fresh packets retain order and timestamps");

    reset();
    for (unsigned i = 0; i < DEV_UPLINK_QUEUE_DEPTH; ++i) assert(dev_send_uplink() == 0);
    tick += 1000U;
    while (queue_count) { job = pop(); dev_send_uplink_job(&job); }
    assert(sdk_calls == 0 && s_tx_session_expired == DEV_UPLINK_QUEUE_DEPTH);
    assert(s_tx_dropped == DEV_UPLINK_QUEUE_DEPTH);
    assert(dev_send_uplink() == 0); job = pop(); dev_send_uplink_job(&job);
    assert(sdk_calls == 1 && s_tx_frames == 1U && sent.payload[0] == DEV_UPLINK_QUEUE_DEPTH + 1U);
    ++s_generation; ++s_tirtc_generation; dev_uplink_reset_session_stats(); tick += 1000U;
    dev_send_uplink_job(&job);
    assert(sdk_calls == 1 && !s_tx_session_expired && s_tx_dropped == DEV_UPLINK_QUEUE_DEPTH);
    assert(!s_tx_session_queue_full && !s_tx_session_sdk_busy);
    puts("PASS: long stall expires backlog, fresh speech resumes, reset preserves totals and ignores expired old-session jobs");

    for (unsigned gate = 0; gate < 7U; ++gate) {
        reset(); assert(dev_send_uplink() == 0); job = pop(); tick += 1000U;
        if (gate == 0U) ++s_generation;
        else if (gate == 1U) ++s_tirtc_generation;
        else if (gate == 2U) s_tirtc_release_pending = true;
        else if (gate == 3U) s_state = 8U;
        else if (gate == 4U) s_request_hangup = true;
        else if (gate == 5U) s_request_conn_error = true;
        else s_tirtc_claimed = false;
        dev_send_uplink_job(&job);
        assert(!sdk_calls && !s_tx_dropped && !s_tx_session_expired);
    }
    reset(); assert(dev_send_uplink() == 0); job = pop(); tick = job.frame.ts + 300U;
    sdk_advance_ms = 1000U; sdk_result = TIRTC_E_BUSY;
    dev_send_uplink_job(&job);
    assert(sdk_calls == 1 && s_tx_session_sdk_busy == 1U && s_tx_dropped == 1U && !s_tx_session_expired);
    assert(!s_request_conn_error && !signal_count);
    puts("PASS: session/stop gates precede age accounting; in-flight SDK wait retains original BUSY policy");
    assert(!critical_depth);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='lite-uplink-queue-') as directory:
    directory = Path(directory)
    test_c = directory / 'test.c'
    executable = directory / 'test.exe'
    test_c.write_text(harness, encoding='utf-8')
    subprocess.run(command + ['-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-O0', '-I', str(SDK), str(test_c), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
