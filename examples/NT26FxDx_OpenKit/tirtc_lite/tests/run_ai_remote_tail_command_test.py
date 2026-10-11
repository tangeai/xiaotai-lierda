#!/usr/bin/env python3
"""Exercise the production terminal-audio command filter with real cJSON.

Pass --cjson /path/to/upstream/cJSON-1.7.16/cJSON.c; the SDK ships only its
header and target binary. No parser, production filter, or json_guard is mocked.
Only the bounded RTOS queue and slot ownership are faked. This checks command
handling, not real transport ordering or acoustic playback completion.
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
from run_dev_playback_gain_test import compiler_command

CJSON_SHA256 = "fdfd427d82fadb395076567edf470c80cebee319e38fd417198508fe11ae56e7"

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_guard.h"
#define LIOT_NO_WAIT 0U
#define LIOT_OSI_SUCCESS 0
typedef struct {
    uint16_t length;
    uint32_t cmdw;
    char payload[AI_COMMAND_MAX_BYTES + 1U];
} ai_command_message_t;
static int queue_handle;
static void *s_command_queue = &queue_handle;
static ai_command_message_t s_command_pool[AI_COMMAND_QUEUE_DEPTH];
static struct { unsigned handled; } s_command_diag;
static uint8_t queue_slots[AI_COMMAND_QUEUE_DEPTH];
static bool used[AI_COMMAND_QUEUE_DEPTH];
static unsigned qread, qcount, waits, releases, allocations, cases;
static bool replenish;
static void *count_alloc(size_t size) {
    void *p = malloc(size);
    if (p != NULL) ++allocations;
    return p;
}
static void count_free(void *p) {
    if (p != NULL) { assert(allocations > 0U); --allocations; free(p); }
}
static void enqueue_slot(uint8_t slot) {
    assert(qcount < AI_COMMAND_QUEUE_DEPTH);
    queue_slots[(qread + qcount) % AI_COMMAND_QUEUE_DEPTH] = slot;
    ++qcount;
}
static void put(unsigned slot, uint32_t cmdw, const char *json) {
    size_t length = strlen(json);
    assert(slot < AI_COMMAND_QUEUE_DEPTH && !used[slot]);
    assert(length <= AI_COMMAND_MAX_BYTES);
    used[slot] = true;
    s_command_pool[slot].cmdw = cmdw;
    s_command_pool[slot].length = (uint16_t)length;
    memcpy(s_command_pool[slot].payload, json, length + 1U);
    enqueue_slot((uint8_t)slot);
}
static int liot_rtos_queue_wait(void *q, uint8_t *slot, size_t size,
                                 unsigned timeout) {
    assert(q == &queue_handle && size == sizeof(*slot) && timeout == LIOT_NO_WAIT);
    ++waits;
    if (qcount == 0U) return -1;
    *slot = queue_slots[qread];
    qread = (qread + 1U) % AI_COMMAND_QUEUE_DEPTH;
    --qcount;
    return LIOT_OSI_SUCCESS;
}
static void ai_command_slot_release(uint8_t slot) {
    assert(slot < AI_COMMAND_QUEUE_DEPTH && used[slot]);
    used[slot] = false;
    ++releases;
    /* A producer can immediately reuse a freed slot. A bounded scan must
     * still return after eight dequeues under continuous non-terminal input. */
    if (replenish) put(slot, AI_CMD_WORD, "{\"jsonrpc\":\"2.0\",\"method\":\"device_action\"}");
}
/* Any accidental reuse of the full dispatcher is a test failure, even for a
 * syntactically valid new device_action received after end_session. */
static void ai_handle_device_action(const cJSON *root) {
    (void)root; assert(!"terminal drain dispatched a business action");
}
static void ai_handle_command(const ai_command_message_t *message) {
    (void)message; assert(!"terminal drain dispatched a session command");
}
'''

SUFFIX = r'''
static void reset(void) {
    assert(allocations == 0U);
    memset(s_command_pool, 0, sizeof(s_command_pool));
    memset(used, 0, sizeof(used));
    memset(queue_slots, 0, sizeof(queue_slots));
    s_command_queue = &queue_handle;
    s_command_diag.handled = qread = qcount = waits = releases = 0U;
    replenish = false;
}
static void check_json(const char *json, bool expected) {
    reset();
    put(0U, AI_CMD_WORD, json);
    assert(ai_remote_tail_interrupted() == expected);
    assert(qcount == 0U && releases == 1U && !used[0]);
    assert(s_command_diag.handled == 1U && allocations == 0U);
    ++cases;
}
int main(void) {
    unsigned i;
    char deep[128];
    cJSON_Hooks hooks = {count_alloc, count_free};
    cJSON_InitHooks(&hooks);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}", true);
    check_json(" \r\n{\"jsonrpc\":\"2.0\",\"method\":\"interru\\u0070t\",\"params\":{}}", true);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"device_action\",\"params\":{\"action\":\"call\"}}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"round_end\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"round_start\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"end_session\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"Interrupt\"}", false);
    check_json("{\"Method\":\"interrupt\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":null}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":true}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":123}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":[\"interrupt\"]}", false);
    check_json("{}", false);
    check_json("[]", false);
    check_json("null", false);
    check_json("\"interrupt\"", false);
    check_json("", false);
    check_json("{", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\",}", false);
    /* No malformed envelope may interrupt an otherwise valid tail. */
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"} trailing", false);
    check_json("{\"method\":\"interrupt\"}", false);
    check_json("{\"jsonrpc\":\"1.0\",\"method\":\"interrupt\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"interrupt\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"interrupt\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\",\"result\":{}}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\",\"error\":null}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\\u0000suffix\"}", false);
    check_json("{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"} {}", false);
    memset(deep, '[', DEMO_JSON_MAX_DEPTH + 1U);
    memset(deep + DEMO_JSON_MAX_DEPTH + 1U, ']', DEMO_JSON_MAX_DEPTH + 1U);
    deep[2U * (DEMO_JSON_MAX_DEPTH + 1U)] = '\0';
    check_json(deep, false);

    reset(); put(0U, AI_CMD_WORD, "{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}");
    --s_command_pool[0].length; /* NUL and closing brace exist beyond the bound. */
    assert(!ai_remote_tail_interrupted());
    assert(releases == 1U && allocations == 0U); ++cases;

    reset(); put(0U, AI_CMD_WORD + 1U, "{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}");
    assert(!ai_remote_tail_interrupted());
    assert(releases == 1U && s_command_diag.handled == 0U && allocations == 0U); ++cases;

    reset(); assert(!ai_remote_tail_interrupted());
    assert(waits == 1U && releases == 0U); ++cases;
    reset(); s_command_queue = NULL; assert(!ai_remote_tail_interrupted());
    assert(waits == 0U && releases == 0U); ++cases;

    reset(); enqueue_slot((uint8_t)AI_COMMAND_QUEUE_DEPTH);
    put(0U, AI_CMD_WORD, "{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}");
    assert(ai_remote_tail_interrupted());
    assert(waits == 2U && releases == 1U && qcount == 0U); ++cases;

    reset();
    for (i = 0U; i < AI_COMMAND_QUEUE_DEPTH; ++i)
        put(i, AI_CMD_WORD, i + 1U == AI_COMMAND_QUEUE_DEPTH ?
            "{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}" : "{\"jsonrpc\":\"2.0\",\"method\":\"device_action\"}");
    assert(ai_remote_tail_interrupted());
    assert(waits == AI_COMMAND_QUEUE_DEPTH && releases == AI_COMMAND_QUEUE_DEPTH);
    assert(qcount == 0U && allocations == 0U); ++cases;

    reset();
    for (i = 0U; i < AI_COMMAND_QUEUE_DEPTH; ++i)
        put(i, AI_CMD_WORD, i == 0U ? "{\"jsonrpc\":\"2.0\",\"method\":\"interrupt\"}" :
                                   "{\"jsonrpc\":\"2.0\",\"method\":\"device_action\"}");
    assert(ai_remote_tail_interrupted());
    assert(waits == 1U && releases == 1U && qcount == AI_COMMAND_QUEUE_DEPTH - 1U);
    assert(!used[0] && used[AI_COMMAND_QUEUE_DEPTH - 1U]);
    assert(!ai_remote_tail_interrupted());
    assert(qcount == 0U && releases == AI_COMMAND_QUEUE_DEPTH && allocations == 0U); ++cases;

    reset();
    for (i = 0U; i < AI_COMMAND_QUEUE_DEPTH; ++i)
        put(i, AI_CMD_WORD, "{\"jsonrpc\":\"2.0\",\"method\":\"device_action\"}");
    replenish = true;
    assert(!ai_remote_tail_interrupted());
    assert(waits == AI_COMMAND_QUEUE_DEPTH && releases == AI_COMMAND_QUEUE_DEPTH);
    assert(qcount == AI_COMMAND_QUEUE_DEPTH && allocations == 0U);
    for (i = 0U; i < AI_COMMAND_QUEUE_DEPTH; ++i) assert(used[i]);
    replenish = false;
    assert(!ai_remote_tail_interrupted());
    assert(qcount == 0U && releases == 2U * AI_COMMAND_QUEUE_DEPTH && allocations == 0U); ++cases;
    cJSON_InitHooks(NULL);
    printf("PASS: %u production remote-tail command cases; real cJSON/json_guard, null/malformed JSON, bounded 8-slot scan, releases and no business dispatch\n", cases);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cjson", type=Path, required=True,
                        help="Unmodified upstream cJSON v1.7.16 cJSON.c")
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    if hashlib.sha256(args.cjson.read_bytes()).hexdigest() != CJSON_SHA256:
        parser.error("Expected unmodified upstream cJSON v1.7.16 cJSON.c")
    app = Path(__file__).resolve().parents[1]
    root = app.parents[2]
    source = (app / "src/features/ai_chat.c").read_text(encoding="utf-8")
    constants = []
    for name in ("AI_CMD_WORD", "AI_COMMAND_QUEUE_DEPTH", "AI_COMMAND_MAX_BYTES"):
        match = re.search(r"^#define\s+" + name + r"\s+[^\r\n]+", source, re.M)
        if match is None:
            raise AssertionError("Production constant missing: " + name)
        constants.append(match.group())
    body = ("\n".join(constants) + "\n" + PREFIX +
            function(source, "ai_call_json_is_complete") +
            function(source, "ai_command_json_parse") +
            function(source, "ai_command_is_notification") +
            function(source, "ai_remote_tail_interrupted") + SUFFIX)
    with tempfile.TemporaryDirectory(prefix="lite-ai-remote-tail-command-") as temp:
        c = Path(temp) / "remote-tail-command.c"
        exe = c.with_suffix(".exe")
        c.write_text(body, encoding="utf-8")
        subprocess.run(cc + ["-std=c99", "-O2", "-DCJSON_HIDE_SYMBOLS",
                             "-I", str(app / "include"),
                             "-I", str(root / "components/thirdparty/CJSON"),
                             str(c), str(args.cjson.resolve()), "-o", str(exe), "-lm"],
                       check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
