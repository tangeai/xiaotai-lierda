#!/usr/bin/env python3
"""Exercise production TiRTC shutdown ordering with deterministic RTOS/SDK fakes.

The fake Stop follows the audited NT26 v2.5.0 contract: SYS_STOPPED can be
synchronous, and closing the listener supplies no TiRTC handle-release/join
guarantee. This tests application ownership, not actual radio scheduling.
The separate during-Stop probe is reported as an unresolved SDK limitation,
never counted as a passing safety test. --before verifies the original race.
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

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tirtc_runtime.h"
typedef void *liot_task_t;
typedef void *liot_sem_t;
typedef void *liot_queue_t;
typedef uint8_t uint8;
#define LIOT_NO_WAIT 0U
#define LIOT_OSI_SUCCESS 0
static unsigned clock_ms, critical_depth, entries, mode, injections;
static unsigned stops, uninits, unsafe_uninits, disconnects, queued, passed;
static int stop_result;
static bool probe_active, injecting, notify_stopped, notify_on_wait;
static tirtc_conn_t queued_handle;
#define HANDLE ((tirtc_conn_t)(uintptr_t)0x500U)
static void runtime_on_conn_accepted(tirtc_conn_t);
static void runtime_on_disconnected(tirtc_conn_t);
static void runtime_on_event(int,const void *,int);
static void runtime_signal(void);
static void runtime_process_managed_disconnect(void);
static void runtime_process_rejects(void);
static void inject_accept(void);
static void liot_trace(const char *format, ...) {(void)format;}
static void liot_rtos_enter_critical(void) {
    if (probe_active && !injecting && !critical_depth) {
        ++entries;
        /* Shutdown's entry critical section, then its three drain queries.
         * The next entry is the first Stop commit. Inject BEFORE locking. */
        if (mode==1U && entries==5U) inject_accept();
    }
    ++critical_depth;
}
static void liot_rtos_exit_critical(void) {assert(critical_depth);--critical_depth;}
static uint32_t liot_rtos_get_running_time(void) {return clock_ms;}
static void liot_rtos_task_sleep_ms(uint32_t ms) {clock_ms+=ms;}
static int liot_rtos_semaphore_release(liot_sem_t s) {(void)s;return 0;}
static int liot_rtos_semaphore_wait(liot_sem_t s,uint32_t ms) {
    (void)s;clock_ms+=ms;
    if(notify_on_wait) {notify_on_wait=false;runtime_on_event(TIRTC_EVENT_SYS_STOPPED,NULL,0);}
    return 0;
}
static int liot_rtos_queue_release(liot_queue_t q,uint32_t n,uint8 *p,uint32_t wait) {
    (void)q;(void)wait;assert(!queued&&n==sizeof(queued_handle));
    memcpy(&queued_handle,p,n);queued=1;return 0;
}
static int liot_rtos_queue_wait(liot_queue_t q,uint8 *p,uint32_t n,uint32_t wait) {
    (void)q;(void)wait;if(!queued)return -1;
    assert(n==sizeof(queued_handle));memcpy(p,&queued_handle,n);queued=0;return 0;
}
static int runtime_owner_on_accepted(demo_tirtc_owner_e owner,tirtc_conn_t h) {
    (void)owner;(void)h;return -1;
}
static void runtime_owner_on_disconnected(demo_tirtc_owner_e owner,tirtc_conn_t h) {
    (void)owner;(void)h;
}
static void runtime_snapshot_listeners(
    const demo_tirtc_listener_t *feature[DEMO_TIRTC_FEATURE_COUNT],
    const demo_tirtc_incoming_listener_t **incoming) {
    memset(feature,0,sizeof(*feature)*DEMO_TIRTC_FEATURE_COUNT);*incoming=NULL;
}
'''

SUFFIX = r'''
static void inject_accept(void) {
    assert(!critical_depth);injecting=true;++injections;
    runtime_on_conn_accepted(HANDLE);injecting=false;
    assert(s_reject_pending==1U||s_connection.closing==HANDLE);
}
int TiRtcStop(void) {
    assert(!critical_depth);++stops;
    if(mode==2U)inject_accept();
    if(notify_stopped)runtime_on_event(TIRTC_EVENT_SYS_STOPPED,NULL,0);
    return stop_result;
}
void TiRtcUninit(void) {
    assert(!critical_depth);++uninits;
    if(s_reject_pending||s_connection.closing||s_accept_callbacks_pending)++unsafe_uninits;
}
int TiRtcDisconnect(tirtc_conn_t h) {
    /* Deliberately prohibit the unsafe 'fix' of disconnecting after Stop. */
    assert(h==HANDLE&&!stops&&!critical_depth);++disconnects;
    runtime_on_disconnected(h);return 0;
}
const char *TiRtcGetErrorStr(int error) {(void)error;return "mock";}
static void reset(void) {
    memset(&s_connection,0,sizeof(s_connection));
    memset(&s_connect_context,0,sizeof(s_connect_context));
    memset(&s_dev_connect_submit,0,sizeof(s_dev_connect_submit));
    memset(s_reject_slots,0,sizeof(s_reject_slots));
    memset(s_retired_session_generation,0,sizeof(s_retired_session_generation));
    s_reject_pending=s_accept_callbacks_pending=0;
    s_admission_busy=s_restart_required=false;s_ready=true;
    s_runtime_sem=(void *)1;s_reject_queue=(void *)2;
    s_sdk_initialized=s_start_submitted=true;
    s_stop_requested=s_started_event=s_stopped_event=false;
    s_error=s_restart_error=0;
    clock_ms=100;critical_depth=entries=mode=injections=0;
    stops=uninits=unsafe_uninits=disconnects=queued=0;
    stop_result=0;probe_active=injecting=notify_on_wait=false;notify_stopped=true;
}
static void pass(const char *name) {++passed;printf("PASS %02u %s\n",passed,name);}
static int shutdown_probe(void) {
    probe_active=true;int ret=runtime_graceful_stop_and_uninit();probe_active=false;return ret;
}
static void precommit_case(bool managed) {
    reset();mode=1;
    if(managed) {
        s_connection.owner=DEMO_TIRTC_OWNER_AI;s_connection.session_generation=7;
    }
    int ret=shutdown_probe();
    assert(injections==1);
#ifdef EXPECT_OLD_GAP
    assert(ret==0&&stops==1&&uninits==1&&unsafe_uninits==1&&!disconnects);
    runtime_reset_after_uninit();
    assert(!s_reject_pending&&!s_connection.closing&&!disconnects);
    printf("EXPECTED NEGATIVE CONTROL: late %s handle tracking erased without Disconnect\n",
           managed?"managed":"unowned");
#else
    assert(ret==TIRTC_E_BUSY&&!stops&&!uninits&&!s_stop_requested&&!disconnects);
    runtime_process_managed_disconnect();runtime_process_rejects();
    assert(disconnects==1&&!s_reject_pending&&!s_connection.closing);
    mode=0;
    assert(shutdown_probe()==0&&stops==1&&uninits==1&&!unsafe_uninits);
    pass(managed?"managed late handle disconnects before retry Stop":
                 "unowned late handle disconnects before retry Stop");
#endif
}
int main(void) {
    precommit_case(false);precommit_case(true);
#ifdef EXPECT_OLD_GAP
    puts("CONFIRMED: original source has the precommit ownership gap");return 0;
#endif
    reset();assert(shutdown_probe()==0&&stops==1&&uninits==1&&!unsafe_uninits);
    pass("ordinary Stop and synchronous SYS_STOPPED preserve ordering");
    reset();s_stopped_event=true;s_start_submitted=false;
    assert(shutdown_probe()==0&&!stops&&uninits==1);
    pass("already stopped runtime is uninitialized without resubmitting Stop");
    reset();s_start_submitted=false;
    assert(shutdown_probe()==0&&!stops&&uninits==1);
    pass("initialized but never started runtime skips Stop");
    reset();s_sdk_initialized=false;s_start_submitted=false;
    assert(shutdown_probe()==0&&!stops&&!uninits);
    pass("uninitialized runtime does no SDK teardown");
    reset();stop_result=-123;notify_stopped=false;
    assert(shutdown_probe()==-123&&stops==1&&!uninits&&!s_stop_requested);
    stop_result=0;notify_stopped=true;
    assert(shutdown_probe()==0&&stops==2&&uninits==1);
    pass("failed Stop retains SDK and permits the existing retry");
    reset();stop_result=-123;
    assert(shutdown_probe()==0&&stops==1&&uninits==1);
    pass("SYS_STOPPED racing a failed Stop return is honored");
    reset();notify_stopped=false;
    assert(shutdown_probe()==DEMO_TIRTC_ERR_TIMEOUT&&stops==1&&!uninits&&s_stop_requested);
    notify_on_wait=true;
    assert(shutdown_probe()==0&&stops==1&&uninits==1);
    pass("missing stop event defers Uninit and subsequent event avoids duplicate Stop");
    reset();s_connection.active=HANDLE;s_connection.active_owner=DEMO_TIRTC_OWNER_AI;
    s_connection.owner=DEMO_TIRTC_OWNER_AI;s_connection.session_generation=7;
    s_connection.active_generation=7;
    assert(shutdown_probe()==0&&disconnects==1&&stops==1&&uninits==1&&!unsafe_uninits);
    pass("preexisting active connection drains through unchanged public Disconnect path");
    reset();mode=2;
    assert(shutdown_probe()==0&&injections==1&&unsafe_uninits==1&&!disconnects);
    puts("KNOWN SDK LIMITATION (not a passed safety case): callback arriving inside Stop\n"
         "still lacks a supported join/release guarantee; no post-Stop Disconnect added.");
    printf("PASS: %u production lifecycle cases; one unresolved SDK-window probe reported separately\n",passed);
    return 0;
}
'''

NAMES = [
    'runtime_deadline_pending', 'runtime_signal', 'runtime_next_generation',
    'runtime_owner_valid', 'runtime_connect_context_clear_locked',
    'runtime_closing_clear_locked', 'runtime_active_clear_locked',
    'runtime_reserve_closing_locked', 'runtime_active_release',
    'runtime_process_managed_disconnect', 'runtime_reject_slot_clear_locked',
    'runtime_reject_later', 'runtime_reject_complete', 'runtime_process_rejects',
    'runtime_on_event', 'runtime_on_conn_accepted', 'runtime_on_disconnected',
    'demo_tirtc_require_restart', 'runtime_connections_drained_locked',
    'runtime_connections_drained_for_stop', 'runtime_begin_graceful_shutdown',
    'runtime_graceful_stop_and_uninit', 'runtime_reset_after_uninit',
]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--before', type=Path, help='original runtime C: expect the known negative control')
    args = parser.parse_args()
    cc = compiler_command(args.cc, parser)
    product = Path(__file__).resolve().parents[1]
    source_path = args.before or product / 'src/core/tirtc_runtime.c'
    source = source_path.read_text(encoding='utf-8')
    constants = '\n'.join(re.findall(r'^#define DEMO_TIRTC_\w+[^\n]*', source, re.M))
    # Omit unused multiline log macro headers from the extracted definitions.
    constants = '\n'.join(x for x in constants.splitlines() if not x.endswith('\\'))
    declarations = source[source.index('static const demo_tirtc_listener_t * volatile'):
                          source.index('static bool runtime_dev_connect_needs_cleanup')]
    functions = []
    for name in NAMES:
        if name == 'runtime_connections_drained_locked' and args.before and name not in source:
            continue
        functions.append(function(source, name))
    unit = PREFIX + '\n' + constants + '\n' + declarations + '\n' + '\n'.join(functions) + SUFFIX
    with tempfile.TemporaryDirectory(prefix='lite-tirtc-stop-gate-') as output:
        code = Path(output) / 'stop_gate.c'
        exe = Path(output) / 'stop_gate.exe'
        code.write_text(unit, encoding='utf-8')
        cmd = cc + ['-std=c99', '-O1', '-UWIN32', '-DHWDEMO_GROUP_ROOM_EN']
        if args.before:
            cmd.append('-DEXPECT_OLD_GAP')
        cmd += ['-I', str(product / 'include'), '-I', str(product / 'sdk/include'), str(code), '-o', str(exe)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()
