#ifndef TIRTC_JPEG_STAGE_H
#define TIRTC_JPEG_STAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef TIRTC_JPEG_STAGE_DIAGNOSTICS
#define TIRTC_JPEG_STAGE_DIAGNOSTICS 0
#endif

#if TIRTC_JPEG_STAGE_DIAGNOSTICS
typedef struct {
    uint32_t count;
    uint32_t wall_ms;
    uint32_t max_ms;
    /* Only populated for the top-level wait probes. These three counters
     * describe the same subset of waits with two valid scheduler stamps. */
    uint32_t run_count;
    uint32_t run_wall_ms;
    uint32_t run_ms;
} tirtc_jpeg_stage_span_t;

typedef struct {
    uint32_t begin_ms;
    uint32_t end_ms;
    tirtc_jpeg_stage_span_t wait[2];
    tirtc_jpeg_stage_span_t coeff;
} tirtc_jpeg_stage_stats_t;

/* Take and stop the lease on the same task, around a decode-only session.
 * All wall times use the RTOS
 * clock, so a wait (or CPU coefficient step) may include task preemption.
 * Scheduler run time includes synchronous hardware waits on the task.
 * Disable per-block coefficient timing for a lower-overhead comparison. */
bool tirtc_jpeg_stage_begin(bool measure_coeff);
bool tirtc_jpeg_stage_snapshot(tirtc_jpeg_stage_stats_t *out);
bool tirtc_jpeg_stage_end(tirtc_jpeg_stage_stats_t *out);
#endif

#endif
