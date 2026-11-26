#ifndef TIRTC_VIDEO_PERF_H
#define TIRTC_VIDEO_PERF_H
#include <stdint.h>
/* Optional TIRTC_VIDEO_PERF_DIAGNOSTICS=1 observations. No test command,
 * text, image or addresses. RTOS wall milliseconds include preemption/waits;
 * these are not CPU utilization or memory/LSPI bandwidth measurements. */
enum {
    TIRTC_VPERF_BLIT, TIRTC_VPERF_RECT, TIRTC_VPERF_GLYPH,
    TIRTC_VPERF_PREPARE, TIRTC_VPERF_WRITE,
    TIRTC_VPERF_PROCESS, TIRTC_VPERF_TIMER, TIRTC_VPERF_HEAP,
    TIRTC_VPERF_COUNT
};
typedef struct { uint32_t calls, sum_ms, max_ms; } tirtc_video_perf_counter_t;
typedef struct {
    uint32_t sample_ms, render_active, render_count, render_sum_ms, render_max_ms;
    uint32_t render_blit_ms, render_rect_ms, render_glyph_ms;
    uint32_t render_prepare_ms, render_write_ms, render_other_ms;
    uint32_t flush_count, flush_pixels, flush_failures, gui_waits, errors, hooks_ready;
    /* Callback spans are inclusive and may nest: never add these as CPU cost. */
    tirtc_video_perf_counter_t span[TIRTC_VPERF_COUNT];
} tirtc_video_perf_snapshot_t;
typedef struct { uint32_t started_ms, serial, kind, depth; } tirtc_video_perf_scope_t;
/* Tokens are lexical, synchronous, used once. Drawing belongs to GUI owner;
 * HEAP belongs to codec worker. Completed render_*_ms partition outermost
 * callbacks exclusively (nested glyph time belongs to its outer rect).
 * Their sum +render_other_ms equals completed render_sum_ms modulo u32.
 * Invalid scopes increment errors and discard the affected render sample. */
tirtc_video_perf_scope_t tirtc_video_perf_begin(unsigned kind);
void tirtc_video_perf_end(tirtc_video_perf_scope_t scope);
void tirtc_video_perf_note_gui_wait(void);
/* RAM-only bounded copy, no LVGL/heap/I/O/task scan. Boot counters wrapu32,
 * maxima are lifetime maxima; the getter never resets observations. */
void tirtc_video_perf_get_snapshot(tirtc_video_perf_snapshot_t *out);
#endif
