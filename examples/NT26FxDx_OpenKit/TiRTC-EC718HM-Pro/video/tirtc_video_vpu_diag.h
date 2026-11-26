#ifndef TIRTC_VIDEO_VPU_DIAG_H
#define TIRTC_VIDEO_VPU_DIAG_H
#include <stdbool.h>
#include <stdint.h>
#ifndef TIRTC_VIDEO_VPU_DIAGNOSTICS
#define TIRTC_VIDEO_VPU_DIAGNOSTICS 0
#endif
#ifndef TIRTC_VIDEO_VPU_YIELD_ENABLE
#define TIRTC_VIDEO_VPU_YIELD_ENABLE 0
#endif
#ifndef TIRTC_VIDEO_VPU_ISOLATION
#define TIRTC_VIDEO_VPU_ISOLATION 0
#endif
#if TIRTC_VIDEO_VPU_DIAGNOSTICS || TIRTC_VIDEO_VPU_YIELD_ENABLE || TIRTC_VIDEO_VPU_ISOLATION
enum { TIRTC_VPU_ENCODE, TIRTC_VPU_INFO, TIRTC_VPU_LINE, TIRTC_VPU_STAGE_COUNT };
typedef struct { uint32_t generation, kind, tick; } tirtc_vpu_stage_t;
typedef struct {
    uint32_t tick, generation, owner, lock_n, failed, yields, yield_ms;
    uint32_t lock_ms, lock_max, in_flight, pending_failed, pending_yields;
    uint32_t reentry, stale;
    uint32_t enc_n, enc_ms, enc_max, info_n, info_ms, info_max;
    uint32_t line_n, line_ms, line_max;
} tirtc_vpu_snapshot_t;
/* Called only by the permanent codec service task, never callbacks or UI. */
void tirtc_vpu_worker_start(void);
void tirtc_vpu_session_begin(uint32_t owner, uint32_t generation);
void tirtc_vpu_session_end(uint32_t generation);
tirtc_vpu_stage_t tirtc_vpu_stage_begin(uint32_t kind);
void tirtc_vpu_stage_end(tirtc_vpu_stage_t scope);
void tirtc_vpu_report(uint32_t generation, unsigned final);
void tirtc_vpu_snapshot(tirtc_vpu_snapshot_t *out);
#if TIRTC_VIDEO_VPU_ISOLATION
/* Lease tracing to a synchronous benchmark task for one measured phase.
 * Returns -1 if the production worker or another phase owns tracing. */
int tirtc_vpu_isolation_begin(uint32_t generation);
/* The armed fixture call retains the selected vendor lock-yield policy. */
int tirtc_vpu_fixture_call_begin(uint32_t owner, uint32_t generation, bool tuned);
void tirtc_vpu_isolation_end(void);
#endif
#endif
#endif
