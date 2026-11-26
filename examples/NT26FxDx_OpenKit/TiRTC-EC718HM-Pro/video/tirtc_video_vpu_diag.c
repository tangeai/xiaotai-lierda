#include "tirtc_video_vpu_diag.h"
#if TIRTC_VIDEO_VPU_DIAGNOSTICS || TIRTC_VIDEO_VPU_YIELD_ENABLE || TIRTC_VIDEO_VPU_ISOLATION
#include "FreeRTOS.h"
#include "task.h"
#include "liot_os.h"
#include "liot_log.h"
#include <stdbool.h>
#include <string.h>

/* Native F9D_D 1.7.0 archive ABI, verified at the relocation and executable
 * call sites. These are linker wrappers, never fixed-address calls.
 * Only the original vendor code acquires/releases/selects the VPU. */
extern void __real_Video_VpuLock(void);
extern uint32_t __real_SIPC_ResLock(uint32_t resource);
#define VPU_RESOURCE 6U
#define VPU_YIELD_BUDGET 8U
#define VPU_TUNED_YIELD_BUDGET 1U
#define VPU_REPORT_MS 5000U
static TaskHandle_t s_worker;
static uint32_t s_generation, s_report_tick;
static tirtc_vpu_snapshot_t s_stats;
static struct {
    uint32_t depth, began, failed, yields, yield_ms, reserved;
} s_lock;

#if TIRTC_VIDEO_VPU_ISOLATION
/* A wrapped vendor call remains the only operation that touches the SIPC
 * semaphore. Static AP references can be observed. The base AP/CP images,
 * local references and Flash-specific entry points can bypass these wrappers. */
extern void __real_SIPC_ResRelease(uint32_t resource);
#define ISO_AP_ACTOR_COUNT 4U
enum { ISO_NO_HOLDER, ISO_BENCH_HOLDER, ISO_AP_FIRST };
typedef struct {
    uint32_t caller, acquired, released, held_ms, held_max, denied_seen;
} iso_ap_actor_t;

typedef struct {
    uint32_t tick, run_ticks, holder, since, yield_budget;
    uint32_t own_acquired, own_released, own_ms, own_max;
    uint32_t ap_acquired, ap_released, ap_ms, ap_max;
    uint32_t seen_ap_denied, unknown_denied;
    uint32_t opaque_acquire, opaque_release, inconsistent, overflow, overlap;
    iso_ap_actor_t ap[ISO_AP_ACTOR_COUNT];
} iso_trace_t;
static iso_trace_t s_isolation;
static volatile TaskHandle_t s_iso_worker;
#endif

#if TIRTC_VIDEO_VPU_HOST_TEST
extern uint32_t tirtc_vpu_test_ipsr(void);
extern uint32_t tirtc_vpu_test_primask(void);
extern uint32_t tirtc_vpu_test_basepri(void);
#define READ_IPSR() tirtc_vpu_test_ipsr()
#define READ_PRIMASK() tirtc_vpu_test_primask()
#define READ_BASEPRI() tirtc_vpu_test_basepri()
#else
#if !defined(__arm__) && !defined(__thumb__)
#error "VPU diagnostics require the audited ARM target or explicit host fakes"
#endif
static uint32_t read_ipsr(void) { uint32_t v; __asm volatile("mrs %0, ipsr":"=r"(v)); return v; }
static uint32_t read_primask(void) { uint32_t v; __asm volatile("mrs %0, primask":"=r"(v)); return v; }
static uint32_t read_basepri(void) { uint32_t v; __asm volatile("mrs %0, basepri":"=r"(v)); return v; }
#define READ_IPSR() read_ipsr()
#define READ_PRIMASK() read_primask()
#define READ_BASEPRI() read_basepri()
#endif

static bool worker_context(void)
{
    TaskHandle_t worker = s_worker;
#if TIRTC_VIDEO_VPU_ISOLATION
    if (s_iso_worker) worker = s_iso_worker;
#endif
    if (!worker || READ_IPSR() || READ_PRIMASK() || READ_BASEPRI()) return false;
    if (xTaskGetCurrentTaskHandle() != worker) return false;
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}
static uint32_t stamp(void) { return liot_rtos_get_running_time(); }

#if TIRTC_VIDEO_VPU_ISOLATION
static bool isolation_task_context(void)
{
    return !READ_IPSR() && !READ_PRIMASK() && !READ_BASEPRI() &&
           xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/* A successful acquisition and release are observed at their real SIPC
 * boundaries. If the call context cannot be sampled we lose attribution,
 * but never change the original call's return, timing policy or ownership. */
static void isolation_acquired(uint32_t generation, uint32_t caller)
{
    if (!isolation_task_context()) {
        if (!s_iso_worker || generation != s_generation) return;
        s_isolation.opaque_acquire = 1U;
        return;
    }

    bool own = xTaskGetCurrentTaskHandle() == s_iso_worker;
    uint32_t now = stamp();
    liot_rtos_enter_critical();
    if (!s_iso_worker || generation != s_generation) { liot_rtos_exit_critical(); return; }
    if (s_isolation.holder) ++s_isolation.inconsistent;
    if (own) {
        ++s_isolation.own_acquired;
        s_isolation.holder = ISO_BENCH_HOLDER;
    } else {
        unsigned i;
        ++s_isolation.ap_acquired;
        for (i = 0U; i < ISO_AP_ACTOR_COUNT; ++i) {
            iso_ap_actor_t *actor = &s_isolation.ap[i];
            if (actor->acquired && actor->caller != caller) continue;
            actor->caller = caller;
            ++actor->acquired;
            break;
        }
        if (i == ISO_AP_ACTOR_COUNT) ++s_isolation.overflow;
        s_isolation.holder = ISO_AP_FIRST + i;
    }
    s_isolation.since = now;
    liot_rtos_exit_critical();
}

static void isolation_releasing(uint32_t generation)
{
    if (!isolation_task_context()) {
        if (!s_iso_worker || generation != s_generation) return;
        /* A release from a context we cannot sample ends any visible claim.
         * The real hardware release still follows exactly once. */
        s_isolation.holder = ISO_NO_HOLDER;
        s_isolation.opaque_release = 1U;
        return;
    }

    uint32_t now = stamp();
    liot_rtos_enter_critical();
    if (!s_iso_worker || generation != s_generation) { liot_rtos_exit_critical(); return; }
    uint32_t holder = s_isolation.holder;
    uint32_t elapsed = now - s_isolation.since;
    if (holder == ISO_BENCH_HOLDER) {
        ++s_isolation.own_released;
        s_isolation.own_ms += elapsed;
        if (elapsed > s_isolation.own_max) s_isolation.own_max = elapsed;
    } else if (holder >= ISO_AP_FIRST) {
        ++s_isolation.ap_released;
        s_isolation.ap_ms += elapsed;
        if (elapsed > s_isolation.ap_max) s_isolation.ap_max = elapsed;
        uint32_t index = holder - ISO_AP_FIRST;
        if (index < ISO_AP_ACTOR_COUNT) {
            iso_ap_actor_t *actor = &s_isolation.ap[index];
            ++actor->released;
            actor->held_ms += elapsed;
            if (elapsed > actor->held_max) actor->held_max = elapsed;
        }
    } else {
        s_isolation.opaque_release = 1U;
    }

    s_isolation.holder = ISO_NO_HOLDER;
    liot_rtos_exit_critical();
}

static uint32_t isolation_runtime_ticks(TaskHandle_t task)
{
    TaskStatus_t status = {0};
    /* Same pinned base API as the product task sampler. The RTOS counter
     * includes completed scheduling slices. It is coarse at both edges. */
    vTaskGetInfo(task, &status, pdFALSE, eInvalid);
    return status.ulRunTimeCounter;
}

static int isolation_begin(uint32_t owner, uint32_t generation, unsigned yield_budget)
{
    if (!generation || !isolation_task_context()) return -1;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (!task) return -1;
    uint32_t run_ticks = isolation_runtime_ticks(task);
    uint32_t now = stamp();

    liot_rtos_enter_critical();
    if (s_iso_worker || s_generation || s_lock.depth) {
        liot_rtos_exit_critical();
        return -1;
    }
    memset(&s_isolation, 0, sizeof(s_isolation));
    memset(&s_stats, 0, sizeof(s_stats));
    s_isolation.tick = now;
    s_isolation.run_ticks = run_ticks;
    s_isolation.yield_budget = yield_budget;
    s_stats.generation = generation;
    s_stats.owner = owner;
    s_generation = generation;
    s_report_tick = now;
    s_iso_worker = task;
    liot_rtos_exit_critical();
    return 0;
}

int tirtc_vpu_isolation_begin(uint32_t generation)
{
    return isolation_begin(0U, generation, 0U);
}

int tirtc_vpu_fixture_call_begin(uint32_t owner, uint32_t generation, bool tuned)
{
    if (!owner) return -1;
    const unsigned yield_budget = tuned ? VPU_TUNED_YIELD_BUDGET : VPU_YIELD_BUDGET;
    int result = isolation_begin(owner, generation, yield_budget);
#if TIRTC_VIDEO_VPU_YIELD_ENABLE
    if (!result)
        liot_trace("[V12-VPU-POLICY] gen=%lu baseline_limit=%u active_limit=%u sleep_ms=1\r\n",
                   (unsigned long)generation, VPU_YIELD_BUDGET, yield_budget);
#endif
    return result;
}

void tirtc_vpu_isolation_end(void)
{
    if (!s_iso_worker || !worker_context()) return;
    if (s_stats.owner) tirtc_vpu_report(s_generation, 1U);
    uint32_t now = stamp();
    uint32_t run_ticks = isolation_runtime_ticks(s_iso_worker);

    liot_rtos_enter_critical();
    uint32_t generation = s_generation;
    uint32_t in_lock = s_lock.depth;
    const tirtc_vpu_snapshot_t stats = s_stats;
    const iso_trace_t phase = s_isolation;
    s_iso_worker = NULL;
    s_generation = 0U;
    liot_rtos_exit_critical();

    /* Counters are frozen before printing. Normal product tracing remains
     * attached to its original worker and does not consume these counters. */
#define ISO_UL(value) ((unsigned long)(value))
    liot_trace("%s gen=%lu wall_ms=%lu task_ticks=%lu lock_n=%lu lock_ms=%lu lock_max=%lu "
               "denied=%lu ap_denied=%lu unknown_denied=%lu own_acq=%lu own_rel=%lu "
               "own_ms=%lu own_max=%lu other_acq=%lu other_rel=%lu other_ms=%lu other_max=%lu "
               "in_lock=%lu held_slot=%lu incomplete=%lu opaque_acq=%lu opaque_rel=%lu overflow=%lu overlap=%lu\r\n",
               stats.owner ? "[V12-VPU-OWNER]" : "[ISO-VPU]",
               ISO_UL(generation), ISO_UL(now - phase.tick), ISO_UL(run_ticks - phase.run_ticks),
               ISO_UL(stats.lock_n), ISO_UL(stats.lock_ms), ISO_UL(stats.lock_max),
               ISO_UL(stats.failed), ISO_UL(phase.seen_ap_denied), ISO_UL(phase.unknown_denied),
               ISO_UL(phase.own_acquired), ISO_UL(phase.own_released),
               ISO_UL(phase.own_ms), ISO_UL(phase.own_max),
               ISO_UL(phase.ap_acquired), ISO_UL(phase.ap_released),
               ISO_UL(phase.ap_ms), ISO_UL(phase.ap_max),
               ISO_UL(in_lock), ISO_UL(phase.holder), ISO_UL(phase.inconsistent),
               ISO_UL(phase.opaque_acquire), ISO_UL(phase.opaque_release),
               ISO_UL(phase.overflow), ISO_UL(phase.overlap));
    for (unsigned i = 0U; i < ISO_AP_ACTOR_COUNT; ++i) {
        const iso_ap_actor_t *actor = &phase.ap[i];
        if (!actor->acquired) continue;
        liot_trace("%s gen=%lu caller=%lu acq=%lu rel=%lu held_ms=%lu held_max=%lu denied=%lu\r\n",
                   stats.owner ? "[V12-VPU-AP]" : "[ISO-VPU-AP]",
                   ISO_UL(generation), ISO_UL(actor->caller), ISO_UL(actor->acquired), ISO_UL(actor->released),
                   ISO_UL(actor->held_ms), ISO_UL(actor->held_max), ISO_UL(actor->denied_seen));
    }
#undef ISO_UL
}
#endif
void tirtc_vpu_worker_start(void)
{
    /* Registered by task() itself, before the creator publishes s_task. */
    if (!READ_IPSR() && !READ_PRIMASK() && !READ_BASEPRI() &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && !s_worker)
        s_worker = xTaskGetCurrentTaskHandle();
}
void tirtc_vpu_session_begin(uint32_t owner, uint32_t generation)
{
#if TIRTC_VIDEO_VPU_ISOLATION
    if (s_iso_worker) {
        s_isolation.overlap = 1U;
        return;
    }
#endif
    if (!generation || !worker_context()) return;
    liot_rtos_enter_critical();
#if TIRTC_VIDEO_VPU_ISOLATION
    if (s_iso_worker) { s_isolation.overlap = 1U; liot_rtos_exit_critical(); return; }
#endif
    if (s_lock.depth) { ++s_stats.stale; liot_rtos_exit_critical(); return; }
    memset(&s_stats, 0, sizeof(s_stats));
    s_generation = generation; s_stats.generation = generation; s_stats.owner = owner;
    s_report_tick = stamp();
    liot_rtos_exit_critical();
}
void tirtc_vpu_snapshot(tirtc_vpu_snapshot_t *out)
{
    if (!out) return;
    liot_rtos_enter_critical();
    *out = s_stats; out->tick = stamp();
    out->in_flight = s_lock.depth ? 1U : 0U;
    out->pending_failed = s_lock.failed; out->pending_yields = s_lock.yields;
    liot_rtos_exit_critical();
}
void __wrap_Video_VpuLock(void)
{
    if (!s_generation || !worker_context()) { __real_Video_VpuLock(); return; }
    liot_rtos_enter_critical();
    if (s_lock.depth) {
        ++s_lock.depth; ++s_stats.reentry;
        liot_rtos_exit_critical();
        __real_Video_VpuLock();
        liot_rtos_enter_critical(); --s_lock.depth; liot_rtos_exit_critical();
        return;
    }
    s_lock.depth = 1U; s_lock.began = stamp();
    s_lock.failed = s_lock.yields = s_lock.yield_ms = s_lock.reserved = 0U;
    liot_rtos_exit_critical();
    __real_Video_VpuLock();
    liot_rtos_enter_critical();
    uint32_t elapsed = stamp() - s_lock.began;
    ++s_stats.lock_n; s_stats.failed += s_lock.failed; s_stats.yields += s_lock.yields;
    s_stats.yield_ms += s_lock.yield_ms; s_stats.lock_ms += elapsed;
    if (elapsed > s_stats.lock_max) s_stats.lock_max = elapsed;
    memset(&s_lock, 0, sizeof(s_lock));
    liot_rtos_exit_critical();
}
uint32_t __wrap_SIPC_ResLock(uint32_t resource)
{
    /* Exactly one real hardware call. Zero has acquired the resource:
     * never delay/retry/release it. No register selection is reproduced. */
#if TIRTC_VIDEO_VPU_ISOLATION
    uint32_t generation = resource == VPU_RESOURCE && s_iso_worker ? s_generation : 0U;
#endif
    uint32_t result = __real_SIPC_ResLock(resource);
#if TIRTC_VIDEO_VPU_ISOLATION
    if (!result && generation) {
        uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
        isolation_acquired(generation, (uint32_t)caller);
    }
#endif
    if (!result || resource != VPU_RESOURCE || !s_generation ||
        s_lock.depth != 1U || !worker_context()) return result;
    liot_rtos_enter_critical(); ++s_lock.failed;
#if TIRTC_VIDEO_VPU_ISOLATION
    if (s_iso_worker) {
        uint32_t holder = s_isolation.holder;
        if (holder >= ISO_AP_FIRST) {
            ++s_isolation.seen_ap_denied;
            uint32_t index = holder - ISO_AP_FIRST;
            if (index < ISO_AP_ACTOR_COUNT) ++s_isolation.ap[index].denied_seen;
        } else {
            ++s_isolation.unknown_denied;
        }
    }
#endif
#if TIRTC_VIDEO_VPU_YIELD_ENABLE
    bool wait = s_lock.reserved < VPU_YIELD_BUDGET;
#if TIRTC_VIDEO_VPU_ISOLATION
    /* The codec-only matrix selects zero. Armed RTC fixture calls select
     * either the normal or the tuned per-acquisition sleep budget. */
    if (s_iso_worker)
        wait = s_lock.reserved < s_isolation.yield_budget;
#endif
    if (wait) ++s_lock.reserved;
#endif
    liot_rtos_exit_critical();
#if TIRTC_VIDEO_VPU_YIELD_ENABLE
    if (wait) {
        if (worker_context()) {
            uint32_t began = stamp();
            liot_rtos_task_sleep_ms(1U);
            liot_rtos_enter_critical();
            ++s_lock.yields; s_lock.yield_ms += stamp() - began;
            liot_rtos_exit_critical();
        } else {
            liot_rtos_enter_critical(); --s_lock.reserved; liot_rtos_exit_critical();
        }
    }
#endif
    return result;
}
#if TIRTC_VIDEO_VPU_ISOLATION
void __wrap_SIPC_ResRelease(uint32_t resource)
{
    if (resource == VPU_RESOURCE && s_iso_worker) isolation_releasing(s_generation);
    __real_SIPC_ResRelease(resource);
}
#endif
tirtc_vpu_stage_t tirtc_vpu_stage_begin(uint32_t kind)
{
    tirtc_vpu_stage_t token = {0, kind, 0};
    if (kind >= TIRTC_VPU_STAGE_COUNT || !s_generation || !worker_context()) return token;
    token.generation = s_generation; token.tick = stamp(); return token;
}
void tirtc_vpu_stage_end(tirtc_vpu_stage_t token)
{
    if (!token.generation || !worker_context()) return;
    liot_rtos_enter_critical();
    if (token.generation != s_generation || token.kind >= TIRTC_VPU_STAGE_COUNT) ++s_stats.stale;
    else {
        uint32_t elapsed = stamp() - token.tick, *n, *ms, *maximum;
        if (token.kind == TIRTC_VPU_ENCODE) { n=&s_stats.enc_n; ms=&s_stats.enc_ms; maximum=&s_stats.enc_max; }
        else if (token.kind == TIRTC_VPU_INFO) { n=&s_stats.info_n; ms=&s_stats.info_ms; maximum=&s_stats.info_max; }
        else { n=&s_stats.line_n; ms=&s_stats.line_ms; maximum=&s_stats.line_max; }
        ++*n; *ms += elapsed; if (elapsed > *maximum) *maximum = elapsed;
    }
    liot_rtos_exit_critical();
}
void tirtc_vpu_report(uint32_t generation, unsigned final)
{
#if TIRTC_VIDEO_VPU_DIAGNOSTICS
    if (!worker_context() || !generation || generation != s_generation) return;
    uint32_t now = stamp();
    if (!final && (uint32_t)(now - s_report_tick) < VPU_REPORT_MS) return;
    s_report_tick = now;
    tirtc_vpu_snapshot_t p; tirtc_vpu_snapshot(&p);
    liot_trace("[VIDEO-VPU] tick=%lu gen=%lu owner=%lu final=%u lock_n=%lu failed=%lu yields=%lu yield_ms=%lu lock_ms=%lu lock_max=%lu in_flight=%lu pending_failed=%lu pending_yields=%lu enc_n=%lu enc_ms=%lu enc_max=%lu info_n=%lu info_ms=%lu info_max=%lu line_n=%lu line_ms=%lu line_max=%lu reentry=%lu stale=%lu\r\n",
        (unsigned long)p.tick,(unsigned long)p.generation,(unsigned long)p.owner,final,
        (unsigned long)p.lock_n,(unsigned long)p.failed,(unsigned long)p.yields,(unsigned long)p.yield_ms,
        (unsigned long)p.lock_ms,(unsigned long)p.lock_max,(unsigned long)p.in_flight,
        (unsigned long)p.pending_failed,(unsigned long)p.pending_yields,
        (unsigned long)p.enc_n,(unsigned long)p.enc_ms,(unsigned long)p.enc_max,
        (unsigned long)p.info_n,(unsigned long)p.info_ms,(unsigned long)p.info_max,
        (unsigned long)p.line_n,(unsigned long)p.line_ms,(unsigned long)p.line_max,
        (unsigned long)p.reentry,(unsigned long)p.stale);
#else
    (void)generation; (void)final;
#endif
}
void tirtc_vpu_session_end(uint32_t generation)
{
    if (!worker_context() || !generation || generation != s_generation) return;
    liot_rtos_enter_critical();
    if (s_lock.depth) { ++s_stats.stale; liot_rtos_exit_critical(); return; }
    liot_rtos_exit_critical();
    tirtc_vpu_report(generation, 1U);
    liot_rtos_enter_critical();
    if (!s_lock.depth) s_generation = 0U;
    liot_rtos_exit_critical();
}
#endif
