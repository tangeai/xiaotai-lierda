#!/usr/bin/env python3
"""Check the production AI dispatcher against real cJSON and wire envelopes.

Only stateful business/audio side effects are faked; parsing, format checks,
and dispatch decisions come from ai_chat.c. No device or network is used.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from run_aec_uplink_guard_test import function
from run_ai_remote_tail_command_test import CJSON_SHA256
from run_dev_playback_gain_test import compiler_command

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_guard.h"
#define liot_trace(...) ((void)0)
#define AI_ERR_START_REJECTED (-20)
#define AI_ERR_AUDIO_FORMAT (-21)
typedef struct {
    uint16_t length;
    uint32_t cmdw;
    char payload[AI_COMMAND_MAX_BYTES + 1U];
} ai_command_message_t;
static struct { unsigned handled, unknown; } s_command_diag;
static char s_start_request_id[] = "nt26-ai-7";
static bool s_start_response_done, s_server_speaking, s_remote_ended;
static int s_start_response_error, s_audio_lease;
static unsigned s_last_audio_ms, s_play_pending_samples, s_prebuffer_start_ms;
static unsigned actions, stops, discards, resets, allocations, cases;
static unsigned queued_audio, queued_at_stop;
static bool reply_during_stop, s_capture_timeout_active;
static void *count_alloc(size_t size) {
    void *p = malloc(size);
    if (p != NULL) ++allocations;
    return p;
}
static void count_free(void *p) {
    if (p != NULL) { assert(allocations); --allocations; free(p); }
}
static void ai_diag_command(const ai_command_message_t *m, const cJSON *r) {
    (void)m; (void)r;
}
static void ai_handle_device_action(const cJSON *r) { (void)r; ++actions; }
static unsigned liot_rtos_get_running_time(void) { return 1234U; }
static int demo_ai_audio_stop(void *lease) {
    (void)lease; ++stops; queued_at_stop = queued_audio;
    /* Model a new reply arriving while the driver drains the old playback. */
    if (reply_during_stop) queued_audio |= 2U;
    return 0;
}
static void ai_discard_audio_queue(void) { ++discards; queued_audio = 0U; }
static void demo_ai_opus_reset(void) { ++resets; }
'''

SUFFIX = r'''
static void reset(void) {
    assert(allocations == 0U);
    s_command_diag.handled = s_command_diag.unknown = 0U;
    s_start_response_done = s_server_speaking = s_remote_ended = false;
    s_start_response_error = 99;
    s_play_pending_samples = 320U; s_prebuffer_start_ms = 10U;
    s_last_audio_ms = actions = stops = discards = resets = 0U;
    queued_audio = 1U; queued_at_stop = 0U; reply_during_stop = false;
    s_capture_timeout_active = true;
}
static void dispatch_bytes(const char *json, size_t length) {
    ai_command_message_t m;
    assert(length <= AI_COMMAND_MAX_BYTES);
    memset(&m, 0, sizeof(m));
    m.cmdw = AI_CMD_WORD; m.length = (uint16_t)length;
    memcpy(m.payload, json, length);
    ai_handle_command(&m);
    assert(allocations == 0U);
}
static void dispatch(const char *json) { dispatch_bytes(json, strlen(json)); }
static void ignored(const char *json) {
    reset(); dispatch(json);
    assert(!s_start_response_done && !s_remote_ended && !s_server_speaking);
    assert(!actions && !stops && !discards && !resets);
    assert(s_play_pending_samples == 320U && s_prebuffer_start_ms == 10U);
    ++cases;
}
int main(void) {
    cJSON_Hooks hooks = {count_alloc, count_free};
    const char *end = "{\"jsonrpc\":\"2.0\",\"method\":\"end_session\"}";
    const char *start = "{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\",\"result\":{\"session_id\":\"s\"}}";
    char wire[512];
    cJSON_InitHooks(&hooks);
    reset(); dispatch(start); /* Old servers may omit negotiated formats. */
    assert(s_start_response_done && s_start_response_error == 0); ++cases;
    reset(); dispatch("{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\",\"result\":{\"session_id\":\"s\",\"input_audio\":{\"codec\":\"opus\",\"sample_rate\":16000,\"channels\":1},\"output_audio\":{\"codec\":\"opus\",\"sample_rate\":16000,\"channels\":1}}}");
    assert(s_start_response_done && s_start_response_error == 0); ++cases;
    reset(); dispatch("{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\",\"result\":{\"session_id\":\"s\",\"output_audio\":{\"codec\":\"pcm\",\"sample_rate\":8000,\"channels\":1}}}");
    assert(s_start_response_done && s_start_response_error == AI_ERR_AUDIO_FORMAT); ++cases;
    reset(); dispatch("{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\",\"error\":{\"code\":-32602,\"message\":\"bad\"}}");
    assert(s_start_response_done && s_start_response_error == AI_ERR_START_REJECTED); ++cases;
    ignored("{\"id\":\"nt26-ai-7\",\"result\":{\"session_id\":\"s\"}}");
    ignored("{\"jsonrpc\":\"1.0\",\"id\":\"nt26-ai-7\",\"result\":{\"session_id\":\"s\"}}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"wrong\",\"result\":{\"session_id\":\"s\"}}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"session_id\":\"s\"}}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\",\"method\":null,\"result\":{\"session_id\":\"s\"}}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\",\"method\":3,\"result\":{\"session_id\":\"s\"}}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\",\"result\":{\"session_id\":\"s\"},\"error\":null}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"nt26-ai-7\"}");
    reset(); dispatch(end); assert(s_remote_ended); ++cases;
    reset(); dispatch_bytes(end, strlen(end) + 1U); assert(s_remote_ended); ++cases;
    snprintf(wire, sizeof(wire), " \r\n%s \t\r\n", end);
    reset(); dispatch_bytes(wire, strlen(wire) + 1U); assert(s_remote_ended); ++cases;
    ignored("{\"method\":\"end_session\"}");
    ignored("{\"jsonrpc\":2,\"method\":\"end_session\"}");
    ignored("{\"jsonrpc\":\"2.0\\u0000extra\",\"method\":\"end_session\"}");
    ignored("{\"jsonrpc\":\"2.0\",\"method\":\"end_session\\u0000extra\"}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"end_session\"}");
    ignored("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"end_session\"}");
    ignored("{\"jsonrpc\":\"2.0\",\"method\":\"end_session\",\"result\":{}}");
    ignored("{\"jsonrpc\":\"2.0\",\"method\":\"end_session\",\"error\":null}");
    snprintf(wire, sizeof(wire), "%s trailing", end); ignored(wire);
    snprintf(wire, sizeof(wire), "%s {}", end); ignored(wire);
    memcpy(wire, end, strlen(end) + 1U); memcpy(wire + strlen(end) + 1U, "{}", 2U);
    reset(); dispatch_bytes(wire, strlen(end) + 3U); assert(!s_remote_ended); ++cases;
    reset(); dispatch_bytes(end, strlen(end) - 1U); assert(!s_remote_ended); ++cases;
    ignored("[]"); ignored("null"); ignored("{"); ignored("");
    reset(); dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"round_start\"}");
    assert(s_server_speaking && s_last_audio_ms == 1234U); ++cases;
    dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"round_end\"}");
    assert(!s_server_speaking && !stops); ++cases;
    ignored("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"round_start\"}");
    reset(); dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}");
    assert(stops == 1U && discards == 1U && resets == 1U);
    assert(!s_play_pending_samples && !s_prebuffer_start_ms); ++cases;
    reset(); reply_during_stop = true;
    dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}");
    assert(stops == 1U && discards == 1U && resets == 1U);
    assert(queued_at_stop == 0U && queued_audio == 2U);
    assert(!s_play_pending_samples && !s_prebuffer_start_ms);
#if DEMO_AI_AUDIO_AEC_ENABLE
    assert(!s_capture_timeout_active);
#endif
    ++cases;
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"interrupt\"}");
    ignored("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\",\"error\":{}}");
    reset(); dispatch("{\"jsonrpc\":\"2.0\",\"id\":\"call\",\"method\":\"device_action\",\"params\":{\"action\":\"call\"}}");
    assert(actions == 1U); ++cases;
    ignored("{\"jsonrpc\":\"2.0\",\"id\":\"call\",\"method\":\"device_action\",\"result\":{}}");
    reset(); dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"caption\",\"params\":{\"text\":\"\\u4f60\\u597d\\\\u0000\"}}");
    assert(s_command_diag.unknown == 1U && !s_remote_ended); ++cases;
    cJSON_InitHooks(NULL);
    printf("PASS: %u production AI protocol cases with real cJSON; compatible formats, bounded complete messages and controlled side effects\n", cases);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cjson", type=Path, required=True)
    parser.add_argument("--source", type=Path, help="Optional baseline ai_chat.c for negative regression")
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    if hashlib.sha256(args.cjson.read_bytes()).hexdigest() != CJSON_SHA256:
        parser.error("Expected unmodified upstream cJSON v1.7.16 cJSON.c")
    app = Path(__file__).resolve().parents[1]
    root = app.parents[2]
    source = (args.source or app / "src/features/ai_chat.c").read_text(encoding="utf-8")
    constants = []
    for name in ("AI_CMD_WORD", "AI_COMMAND_MAX_BYTES"):
        constants.append(re.search(r"^#define\s+" + name + r"\s+[^\r\n]+", source, re.M).group())
    names = ["ai_json_audio_validate_optional", "ai_call_json_is_complete"]
    for name in ("ai_command_json_parse", "ai_command_is_notification"):
        if re.search(r"static\s+[^;{}]+\b" + name + r"\s*\(", source):
            names.append(name)
    names.append("ai_handle_command")
    body = "\n".join(constants) + PREFIX + "\n".join(function(source, n) for n in names) + SUFFIX
    with tempfile.TemporaryDirectory(prefix="lite-ai-command-protocol-") as temp:
        c = Path(temp) / "ai-command-protocol.c"
        exe = c.with_suffix(".exe")
        c.write_text(body, encoding="utf-8")
        for enabled in (0, 1):
            subprocess.run(cc + ["-std=c99", "-O2", "-DCJSON_HIDE_SYMBOLS",
                                f"-DDEMO_AI_AUDIO_AEC_ENABLE={enabled}",
                                "-I", str(app / "include"),
                                "-I", str(root / "components/thirdparty/CJSON"),
                                str(c), str(args.cjson.resolve()), "-o", str(exe), "-lm"], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
