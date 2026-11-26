/* SPDX-License-Identifier: Apache-2.0
 * Lierda V04 display and touch adaptation.
 * Derived from the board-tested official demo_watch ports:
 *   examples/NT26FxDx_OpenKit/demo/src/demo_watch/framework/platform/
 *     board/board_lierda_watch.c
 *     display/app_display_port_lvgl.c
 * Upstream: https://github.com/lierda-iot/CAT1.bis_OpenCPU
 * The original sources and project license remain in this complete SDK.
 * LCD/TP drivers retain their Lierda copyright notices.
 */
#include "tirtc_port.h"
#include "tirtc_rotation.h"
#include "tirtc_lcd_landscape.h"
#include "tirtc_lcd_rounder.h"
#include "tirtc_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "liot_gpio2.h"
#include "liot_lcd.h"
#include "liot_log.h"
#include "../tirtc_log.h"
#include "liot_os.h"
#include "liot_sleep.h"
#include "liot_tp.h"
#include "lvgl.h"
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
#include "tirtc_video_perf.h"
#endif

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0
#error "The verified ST7789 transport requires native RGB565"
#endif

LIOT_ADD_DISPLAY(liot_st7789_dev);
LIOT_ADD_TP_DEV(g_liot_tp_ft6336);

#define PORT_TICK_MS 1U
#define PORT_HANDLER_MS 10U
#define PORT_TASK_STACK (12U * 1024U)
/* Camera/codec run at 11; audio remains above both at 12. Software scaling
 * must not delay completed camera DMA until the next redraw. */
#define PORT_TASK_PRIORITY 10U
#define PORT_TOUCH_PRESSED (1UL << 31)
#define PORT_VIDEO_REPORT_MS 5000U

static liot_task_t s_ui_task;
static liot_sem_t s_ready_sem;
static liot_timer_t s_tick_timer;
static liot_mutex_t s_state_mutex;
static liot_lcd_handle_t s_lcd;
static liot_tp_handle_t s_touch;
static lv_disp_drv_t s_display_driver;
static lv_disp_draw_buf_t s_draw_buffer;
static lv_indev_drv_t s_input_driver;
static lv_color_t *s_pixels;
/* The SDK retains this pointer. Preserve the original descriptor (also used
 * by the proven touch calibration); native dimensions do not get swapped. */
static liot_hal_lcdDev_t s_lcd_device;
/* One aligned 32-bit store publishes an entire touch sample to the UI task. */
static volatile uint32_t s_touch_sample;
static volatile uint8_t s_requested_brightness = 100U;
static volatile int s_start_result = -1;
/* GUI-owned fixed-size counters. Times include higher-priority preemption;
 * handler includes prepare/write, so these are not additive CPU costs. */
typedef struct { uint32_t sum, max; } port_elapsed_t;
static struct {
    uint32_t tick, loops, flushes, pixels, failures;
    port_elapsed_t process, handler, prepare, write;
    tirtc_ui_video_stats_t previous;
} s_video_stats;

#if TIRTC_VIDEO_PERF_DIAGNOSTICS
static tirtc_video_perf_snapshot_t s_perf;
static uint32_t s_perf_serial, s_perf_depth, s_perf_started;
static uint32_t s_perf_children[5], s_perf_report_tick;
static bool s_perf_invalid;
static lv_draw_ctx_t *s_perf_context;
static void (*s_perf_rect)(lv_draw_ctx_t *,const lv_draw_rect_dsc_t *,const lv_area_t *);
static void (*s_perf_glyph)(lv_draw_ctx_t *,const lv_draw_label_dsc_t *,const lv_point_t *,uint32_t);

tirtc_video_perf_scope_t tirtc_video_perf_begin(unsigned kind)
{
    tirtc_video_perf_scope_t scope={0,0,kind,0};
    liot_rtos_enter_critical();
    scope.started_ms=liot_rtos_get_running_time();
    if (kind<TIRTC_VPERF_PROCESS) {
        scope.serial=s_perf.render_active?s_perf_serial:0U;
        scope.depth=++s_perf_depth;
    }
    liot_rtos_exit_critical();
    return scope;
}
void tirtc_video_perf_end(tirtc_video_perf_scope_t scope)
{
    uint32_t elapsed;
    liot_rtos_enter_critical();
    elapsed=liot_rtos_get_running_time()-scope.started_ms;
    if (scope.kind>=TIRTC_VPERF_COUNT) {
        ++s_perf.errors; liot_rtos_exit_critical(); return;
    }
    tirtc_video_perf_counter_t *counter=&s_perf.span[scope.kind];
    ++counter->calls; counter->sum_ms+=elapsed;
    if (elapsed>counter->max_ms) counter->max_ms=elapsed;
    if (scope.kind<TIRTC_VPERF_PROCESS) {
        if (!scope.depth || scope.depth!=s_perf_depth ||
            (scope.serial && (!s_perf.render_active || scope.serial!=s_perf_serial))) {
            ++s_perf.errors; s_perf_invalid=true;
        } else {
            --s_perf_depth;
            if (scope.serial && scope.depth==1U) s_perf_children[scope.kind]+=elapsed;
        }
    }
    liot_rtos_exit_critical();
}
void tirtc_video_perf_note_gui_wait(void)
{
    liot_rtos_enter_critical(); ++s_perf.gui_waits; liot_rtos_exit_critical();
}
void tirtc_video_perf_get_snapshot(tirtc_video_perf_snapshot_t *out)
{
    if (!out) return;
    liot_rtos_enter_critical();
    *out=s_perf; out->sample_ms=liot_rtos_get_running_time();
    liot_rtos_exit_critical();
}
static void port_perf_render_start(lv_disp_drv_t *driver)
{
    (void)driver;
    liot_rtos_enter_critical();
    s_perf_invalid=s_perf.render_active || s_perf_depth!=0U;
    if (s_perf_invalid) ++s_perf.errors;
    if (!++s_perf_serial) ++s_perf_serial;
    s_perf_depth=0U;
    memset(s_perf_children,0,sizeof(s_perf_children));
    s_perf_started=liot_rtos_get_running_time();
    s_perf.render_active=1U;
    liot_rtos_exit_critical();
}
static void port_perf_render_monitor(lv_disp_drv_t *driver,uint32_t lv_elapsed,uint32_t pixels)
{
    uint32_t elapsed; uint64_t children=0;
    (void)driver; (void)lv_elapsed; (void)pixels;
    liot_rtos_enter_critical();
    elapsed=liot_rtos_get_running_time()-s_perf_started;
    for (unsigned i=0;i<5U;++i) children+=s_perf_children[i];
    if (!s_perf.render_active || s_perf_invalid || s_perf_depth || children>elapsed) {
        ++s_perf.errors;
    } else {
        ++s_perf.render_count; s_perf.render_sum_ms+=elapsed;
        if (elapsed>s_perf.render_max_ms) s_perf.render_max_ms=elapsed;
        s_perf.render_blit_ms+=s_perf_children[TIRTC_VPERF_BLIT];
        s_perf.render_rect_ms+=s_perf_children[TIRTC_VPERF_RECT];
        s_perf.render_glyph_ms+=s_perf_children[TIRTC_VPERF_GLYPH];
        s_perf.render_prepare_ms+=s_perf_children[TIRTC_VPERF_PREPARE];
        s_perf.render_write_ms+=s_perf_children[TIRTC_VPERF_WRITE];
        s_perf.render_other_ms+=elapsed-(uint32_t)children;
    }
    s_perf.render_active=0U; s_perf_depth=0U;
    liot_rtos_exit_critical();
}
static void port_perf_rect(lv_draw_ctx_t *context,const lv_draw_rect_dsc_t *desc,const lv_area_t *area)
{
    tirtc_video_perf_scope_t scope=tirtc_video_perf_begin(TIRTC_VPERF_RECT);
    s_perf_rect(context,desc,area);
    tirtc_video_perf_end(scope);
}
static void port_perf_glyph(lv_draw_ctx_t *context,const lv_draw_label_dsc_t *desc,const lv_point_t *pos,uint32_t letter)
{
    tirtc_video_perf_scope_t scope=tirtc_video_perf_begin(TIRTC_VPERF_GLYPH);
    s_perf_glyph(context,desc,pos,letter);
    tirtc_video_perf_end(scope);
}
static void port_perf_install(lv_draw_ctx_t *context)
{
    if (context && context==s_perf_context && context->draw_rect==port_perf_rect &&
        context->draw_letter==port_perf_glyph) return;
    if (!context || s_perf_context || !context->draw_rect || !context->draw_letter) {
        liot_rtos_enter_critical(); ++s_perf.errors; liot_rtos_exit_critical(); return;
    }
    s_perf_context=context; s_perf_rect=context->draw_rect; s_perf_glyph=context->draw_letter;
    context->draw_rect=port_perf_rect; context->draw_letter=port_perf_glyph;
    liot_rtos_enter_critical(); s_perf.hooks_ready=1U; liot_rtos_exit_critical();
}
static void port_perf_report(void)
{
    uint32_t now=liot_rtos_get_running_time(); tirtc_video_perf_snapshot_t p;
    if ((uint32_t)(now-s_perf_report_tick)<PORT_VIDEO_REPORT_MS) return;
    s_perf_report_tick=now; tirtc_video_perf_get_snapshot(&p);
    liot_trace("[VIDEO-PERF] tick=%lu render_n=%lu render_ms=%lu blit_ms=%lu rect_ms=%lu glyph_ms=%lu prepare_ms=%lu write_ms=%lu other_ms=%lu timer_ms=%lu heap_n=%lu heap_ms=%lu wait_n=%lu flush_n=%lu pixels=%lu fail=%lu hooks=%lu err=%lu\r\n",
        (unsigned long)p.sample_ms,(unsigned long)p.render_count,(unsigned long)p.render_sum_ms,
        (unsigned long)p.render_blit_ms,(unsigned long)p.render_rect_ms,(unsigned long)p.render_glyph_ms,
        (unsigned long)p.render_prepare_ms,(unsigned long)p.render_write_ms,(unsigned long)p.render_other_ms,
        (unsigned long)p.span[TIRTC_VPERF_TIMER].sum_ms,(unsigned long)p.span[TIRTC_VPERF_HEAP].calls,
        (unsigned long)p.span[TIRTC_VPERF_HEAP].sum_ms,(unsigned long)p.gui_waits,
        (unsigned long)p.flush_count,(unsigned long)p.flush_pixels,(unsigned long)p.flush_failures,
        (unsigned long)p.hooks_ready,(unsigned long)p.errors);
}
#endif

static void port_elapsed(port_elapsed_t *value, uint32_t started)
{
    uint32_t elapsed = lv_tick_get() - started;
    value->sum += elapsed;
    if (elapsed > value->max) value->max = elapsed;
}

static void port_video_report(void)
{
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    port_perf_report();
#endif
    if (!TIRTC_ENABLE_DEBUG_LOG) return;
    tirtc_ui_video_stats_t current;
    uint32_t now = lv_tick_get(), dt = now - s_video_stats.tick;
    uint32_t published, consumed, lcd_frames;
    if (dt < PORT_VIDEO_REPORT_MS) return;
    tirtc_ui_video_stats(&current);
    published = current.published - s_video_stats.previous.published;
    consumed = current.consumed - s_video_stats.previous.consumed;
    lcd_frames = current.lcd_frames - s_video_stats.previous.lcd_frames;
    if (current.video_visible || s_video_stats.previous.video_visible || published || lcd_frames) {
        /* LVGL owns a separate fixed pool. Inspect it only in this existing
         * five-second report, on the GUI task, never in the frame/flush path.
         * Free/largest are bytes; fragmentation is LVGL's percentage. */
        lv_mem_monitor_t memory;
        lv_mem_monitor(&memory);
        uint32_t fps100 = (uint32_t)((uint64_t)lcd_frames * 100000U / dt);
        TIRTC_LOG_DEBUG("[GUI35] dt=%lu pub=%lu consume=%lu replace=%lu lcd_frames=%lu lcd_fps_x100=%lu flush=%lu pixels=%lu fail=%lu lv_free=%lu lv_largest=%lu lv_frag=%u draw_fast=%lu draw_fallback=%lu",
            (unsigned long)dt, (unsigned long)published, (unsigned long)consumed,
            (unsigned long)(current.replaced - s_video_stats.previous.replaced),
            (unsigned long)lcd_frames, (unsigned long)fps100,
            (unsigned long)s_video_stats.flushes, (unsigned long)s_video_stats.pixels,
            (unsigned long)s_video_stats.failures,
            (unsigned long)memory.free_size, (unsigned long)memory.free_biggest_size,
            (unsigned)memory.frag_pct,
            (unsigned long)(current.draw_fast - s_video_stats.previous.draw_fast),
            (unsigned long)(current.draw_fallback - s_video_stats.previous.draw_fallback));
        TIRTC_LOG_DEBUG("[GUI35] wall_ms=sum/max loops=%lu process=%lu/%lu handler=%lu/%lu prepare=%lu/%lu lspi=%lu/%lu (prepare/lspi within handler)",
            (unsigned long)s_video_stats.loops,
            (unsigned long)s_video_stats.process.sum, (unsigned long)s_video_stats.process.max,
            (unsigned long)s_video_stats.handler.sum, (unsigned long)s_video_stats.handler.max,
            (unsigned long)s_video_stats.prepare.sum, (unsigned long)s_video_stats.prepare.max,
            (unsigned long)s_video_stats.write.sum, (unsigned long)s_video_stats.write.max);
    }
    memset(&s_video_stats, 0, sizeof(s_video_stats));
    s_video_stats.tick = now;
    s_video_stats.previous = current;
}

static void port_flush_complete(lv_disp_drv_t *driver, bool success)
{
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    liot_rtos_enter_critical();
    ++s_perf.flush_count; if (!success) ++s_perf.flush_failures;
    liot_rtos_exit_critical();
#endif
    if (!success) ++s_video_stats.failures;
    tirtc_ui_video_flush_done(success, lv_disp_flush_is_last(driver));
    lv_disp_flush_ready(driver);
}

/* Protect only bounded state/frame/mailbox copies, never LVGL rendering or I/O. */
void tirtc_ui_platform_lock(void)
{
    if (s_state_mutex != NULL) {
        (void)liot_rtos_mutex_lock(s_state_mutex, LIOT_WAIT_FOREVER);
    }
}

void tirtc_ui_platform_unlock(void)
{
    if (s_state_mutex != NULL) (void)liot_rtos_mutex_unlock(s_state_mutex);
}

void tirtc_port_set_brightness(uint8_t percent)
{
    s_requested_brightness = percent > 100U ? 100U : percent;
}

static int port_power_on(void)
{
    LiotSleepModeCfg_t sleep = {LIOT_SLEEP_MODE_NORMAL};

    if (Liot_AonPowerCtl(true) != L_GPIO_ERR_SUCCESS ||
        Liot_SetVoltage(L_DOMAIN_ALL, L_VOLT_3_30V) != L_GPIO_ERR_SUCCESS) {
        liot_trace("[tirtc] 3.3V domain setup failed");
        return -1;
    }
    (void)Liot_SetPinFunc(16, L_PIN_FUNC_0);
    if (Liot_GpioInit(L_GPIO_25, L_IO_OUTPUT, L_IO_HIGH, NULL) != L_GPIO_ERR_SUCCESS) {
        liot_trace("[tirtc] shared 3.3V enable failed");
        return -1;
    }
    /* This rail powers the fitted LCD/touch assembly and other peripherals. */
    liot_rtos_task_sleep_ms(500U);
    if (Liot_SleepSetMode(&sleep) != LIOT_SLEEP_SUCCESS) {
        liot_trace("[tirtc] normal sleep mode setup failed");
        return -1;
    }
    return 0;
}

static void port_lcd_event(void)
{
    /* Synchronous LSPI writes complete before the flush callback returns. */
}

static int port_lcd_init(void)
{
    liot_lcd_config_t cfg;
    /* V04/24031C12: MX+MV+BGR maps logical (x,y) to native (239-y,x),
     * exactly the former MADCTL 0xC8 + software CCW90 composition. */
    uint8_t madctl = 0x68;

    if (liot_st7789_dev.info.width != TIRTC_PHYSICAL_WIDTH ||
        liot_st7789_dev.info.height != TIRTC_PHYSICAL_HEIGHT ||
        liot_st7789_dev.info.direction != LIOT_LCD_DIR_180_ANGLE ||
        liot_st7789_dev.info.color_depth != LIOT_LCD_COLOR_RGB565) {
        liot_trace("[tirtc] unsupported LCD geometry/calibration");
        return -1;
    }
    s_lcd_device = liot_st7789_dev;
    s_lcd_device.info.direction = LIOT_LCD_DIR_90_ANGLE;

    memset(&cfg, 0, sizeof(cfg));
    cfg.interface.type = LIOT_LCD_INTERFACE_LSPI;
    cfg.interface.lspi.num = LIOT_LSPI_PORT2;
    cfg.interface.lspi.cs = LIOT_LSPI_CS0;
    cfg.interface.lspi.speed = LIOT_LSPI_51MHZ;
    cfg.interface.lspi.sync = true;
    cfg.interface.lspi.cb = port_lcd_event;
    cfg.interface.blk.type = LIOT_LCD_BACKLIGHT_GPIO;
    cfg.interface.blk.pin = 103;
    cfg.interface.rst.pin = 78;
    cfg.interface.rst.delay = 100U;
    cfg.lcdDev = &s_lcd_device;
    s_lcd = liot_lcd_init(&cfg);
    if (s_lcd == NULL) {
        liot_trace("[tirtc] ST7789 init failed");
        return -1;
    }
    /* F6D_A transmit_cmd returns boolean 1 on success, unlike the enum
     * declared in the public header; verified against the linked SDK. */
    if ((int)liot_lcd_send_cmd(s_lcd, 0x36, &madctl, sizeof(madctl)) != 1) {
        liot_trace("[tirtc] ST7789 color order setup failed");
        return -1;
    }
    /* SDK init reads the original global direction and initially selects C0.
     * Override it before clearing; clone direction makes clear use 320x240. */
    liot_trace("[tirtc] ST7789 MADCTL=0x68; hardware landscape RGB565");
    if (liot_lcd_clear_screen(s_lcd, BLACK) != LIOT_LCD_OK) {
        liot_trace("[tirtc] ST7789 initial clear failed");
        return -1;
    }
    (void)liot_lcd_set_brightness(s_lcd, s_requested_brightness);
    return 0;
}

static bool port_write_pixels(const tirtc_rotation_area_t *area, uint8_t *pixels)
{
    s_video_stats.pixels += (uint32_t)(area->x2 - area->x1 + 1) *
                           (uint32_t)(area->y2 - area->y1 + 1);
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    liot_rtos_enter_critical();
    s_perf.flush_pixels+=(uint32_t)(area->x2-area->x1+1)*(uint32_t)(area->y2-area->y1+1);
    liot_rtos_exit_critical();
#endif
    uint32_t started = lv_tick_get();
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    tirtc_video_perf_scope_t write_scope=tirtc_video_perf_begin(TIRTC_VPERF_WRITE);
#endif
    liot_lcd_errcode_e result = liot_lcd_write(s_lcd, (uint16_t)area->x1, (uint16_t)area->y1,
                           (uint16_t)area->x2, (uint16_t)area->y2, pixels);
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    tirtc_video_perf_end(write_scope);
#endif
    port_elapsed(&s_video_stats.write, started);
    if (result != LIOT_LCD_OK) {
        liot_trace("[tirtc] LCD flush failed: %d", (int)result);
    }
    return result == LIOT_LCD_OK;
}

#if TIRTC_VIDEO_DIRECT_VIEW_ENABLE
bool tirtc_port_present_call_video(const uint16_t *pixels, bool square)
{
    if (s_lcd == NULL || pixels == NULL || ((uintptr_t)pixels & 3U)) return false;
    const tirtc_rotation_area_t area = square ?
        (tirtc_rotation_area_t){
            TIRTC_WX_VIDEO_VIEW_X, TIRTC_WX_VIDEO_VIEW_Y,
            TIRTC_WX_VIDEO_VIEW_X + TIRTC_WX_VIDEO_VIEW_SIZE - 1,
            TIRTC_WX_VIDEO_VIEW_Y + TIRTC_WX_VIDEO_VIEW_SIZE - 1
        } : (tirtc_rotation_area_t){0, 0, 255, 239};
    /* The packed RGB565 viewport and DMA segment boundaries are 4-byte aligned. */
    ++s_video_stats.flushes;
    bool success = port_write_pixels(&area, (uint8_t *)(uintptr_t)pixels);
    if (!success) ++s_video_stats.failures;
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    liot_rtos_enter_critical();
    ++s_perf.flush_count;
    if (!success) ++s_perf.flush_failures;
    liot_rtos_exit_critical();
#endif
    return success;
}
#endif

static void port_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    tirtc_rotation_area_t logical, clipped;
    int prepared;
    uint32_t started;

    ++s_video_stats.flushes;
    if (s_lcd == NULL || area == NULL || pixels == NULL) {
        port_flush_complete(driver, false);
        return;
    }
    logical.x1 = area->x1; logical.y1 = area->y1;
    logical.x2 = area->x2; logical.y2 = area->y2;
    started = lv_tick_get();
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    tirtc_video_perf_scope_t prepare_scope=tirtc_video_perf_begin(TIRTC_VPERF_PREPARE);
#endif
    prepared = tirtc_lcd_pack_landscape(pixels,
                                       TIRTC_SCREEN_WIDTH * TIRTC_SCREEN_HEIGHT,
                                       &logical, &clipped);
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    tirtc_video_perf_end(prepare_scope);
#endif
    port_elapsed(&s_video_stats.prepare, started);
    if (prepared != TIRTC_ROTATION_OK) {
        if (prepared == TIRTC_ROTATION_INVALID) liot_trace("[tirtc] invalid LCD flush area");
        port_flush_complete(driver, false);
        return;
    }
    port_flush_complete(driver, port_write_pixels(&clipped, (uint8_t *)pixels));
}

static int port_display_register(void)
{
    const uint32_t count = TIRTC_SCREEN_WIDTH * TIRTC_SCREEN_HEIGHT;

    /* Same 153,600-byte allocation; hardware landscape needs no transpose
     * scratch area, second framebuffer or LVGL rotation heap. */
    s_pixels = liot_rtos_malloc(count * sizeof(*s_pixels));
    if (s_pixels == NULL) {
        liot_trace("[tirtc] framebuffer allocation failed");
        return -1;
    }
    lv_disp_draw_buf_init(&s_draw_buffer, s_pixels, NULL, count);
    lv_disp_drv_init(&s_display_driver);
    s_display_driver.hor_res = TIRTC_SCREEN_WIDTH;
    s_display_driver.ver_res = TIRTC_SCREEN_HEIGHT;
    s_display_driver.rotated = LV_DISP_ROT_NONE;
    s_display_driver.sw_rotate = 0;
    /* LVGL 8.3.10 lv_refr.c:638-694 packs every dirty rectangle at buf_act;
     * :705-798 redraws it before flush. Neither mode may preserve old pixels. */
    s_display_driver.direct_mode = 0;
    s_display_driver.full_refresh = 0;
    s_display_driver.draw_buf = &s_draw_buffer;
    s_display_driver.flush_cb = port_flush;
    s_display_driver.rounder_cb = tirtc_lcd_rounder;
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    s_display_driver.render_start_cb=port_perf_render_start;
    s_display_driver.monitor_cb=port_perf_render_monitor;
#endif
    if (lv_disp_drv_register(&s_display_driver) == NULL) {
        liot_trace("[tirtc] LVGL display registration failed");
        liot_rtos_free(s_pixels);
        s_pixels = NULL;
        return -1;
    }
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
    port_perf_install(s_display_driver.draw_ctx);
#endif
    return 0;
}

static void port_touch_event(liot_tp_touch_data_t *data, void *context)
{
    uint16_t x, y;
    bool pressed;

    (void)context;
    if (data == NULL) return;
    if (data->touch_cnt == 0U) {
        s_touch_sample &= ~PORT_TOUCH_PRESSED;
        return;
    }
    /* LVGL pointer input tracks the first finger, never averages two touches. */
    if (!tirtc_touch_to_landscape(data->point[0].x, data->point[0].y,
                                  liot_st7789_dev.info.direction == LIOT_LCD_DIR_180_ANGLE,
                                  &x, &y)) {
        s_touch_sample &= ~PORT_TOUCH_PRESSED;
        return;
    }
    pressed = data->point[0].event == LIOT_TP_EVT_DOWN ||
              data->point[0].event == LIOT_TP_EVT_MOVE;
    s_touch_sample = (uint32_t)x | ((uint32_t)y << 16) |
                     (pressed ? PORT_TOUCH_PRESSED : 0U);
}

static void port_touch_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    uint32_t sample = s_touch_sample;

    (void)driver;
    data->point.x = (lv_coord_t)(sample & 0xFFFFU);
    data->point.y = (lv_coord_t)((sample >> 16) & 0x7FFFU);
    data->state = (sample & PORT_TOUCH_PRESSED) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    data->continue_reading = false;
}

static int port_touch_init(void)
{
    liot_tp_config_t cfg;
    uint8_t info[4] = {0};

    memset(&cfg, 0, sizeof(cfg));
    cfg.interface_type = LIOT_TP_IF_I2C;
    cfg.i2c.num = (liot_i2c_channel_e)1;
    cfg.i2c.addr = 0x38U;
    cfg.i2c.sda = 66;
    cfg.i2c.scl = 57;
    cfg.i2c.sda_func = 2;
    cfg.i2c.scl_func = 3;
    cfg.rst.pin = 28;
    cfg.rst.delay_ms = 100U;
    cfg.rst.active_low = true;
    cfg.int_pin.pin = 6;
    cfg.int_pin.signal = L_INT_EDGE_FALL;
    cfg.int_pin.pull = LIOT_FORCE_PULL_UP;
    cfg.sensor = &g_liot_tp_ft6336;
    cfg.fw_auto_update = false;
    s_touch = liot_tp_init(&cfg);
    if (s_touch == NULL) {
        liot_trace("[tirtc] FT6336 init failed");
        return -1;
    }
    if (liot_tp_get_ic_info(s_touch, info, sizeof(info)) == LIOT_TP_SUCCESS) {
        liot_trace("[tirtc] FT6336 chip=%02x fw=%02x project=%02x", info[0], info[1], info[2]);
    }
    /* New UI receives native LVGL gestures; do not retain watch's global back swipe. */
    if (liot_tp_register_int_callback(s_touch, port_touch_event, NULL, NULL, NULL) != LIOT_TP_SUCCESS ||
        liot_tp_enable_int(s_touch, true) != LIOT_TP_SUCCESS) {
        liot_trace("[tirtc] FT6336 interrupt setup failed");
        return -1;
    }
    lv_indev_drv_init(&s_input_driver);
    s_input_driver.type = LV_INDEV_TYPE_POINTER;
    s_input_driver.read_cb = port_touch_read;
    s_input_driver.disp = lv_disp_get_default();
    if (lv_indev_drv_register(&s_input_driver) == NULL) {
        liot_trace("[tirtc] LVGL touch registration failed");
        return -1;
    }
    return 0;
}

static void port_tick(void *context)
{
    (void)context;
    lv_tick_inc(PORT_TICK_MS);
}

static void port_ui_task(void *context)
{
    int result;
    uint8_t applied_brightness;
    uint32_t heartbeat_tick;

    (void)context;
    liot_trace("[tirtc] init 1/6: LVGL");
    lv_init();
    liot_trace("[tirtc] init 2/6: shared 3.3V power");
    result = port_power_on();
    if (result == 0) {
        liot_trace("[tirtc] init 3/6: ST7789 LCD");
        result = port_lcd_init();
    }
    if (result == 0) {
        liot_trace("[tirtc] init 4/6: framebuffer and display");
        result = port_display_register();
    }
    if (result == 0) {
        liot_trace("[tirtc] init 5/6: FT6336 touch");
        result = port_touch_init();
    }
    if (result == 0) {
        if (liot_rtos_timer_create(&s_tick_timer, LIOT_TimerPeriodic, port_tick, NULL) != LIOT_OSI_SUCCESS ||
            liot_rtos_timer_start(s_tick_timer, PORT_TICK_MS) != LIOT_OSI_SUCCESS) {
            liot_trace("[tirtc] LVGL tick setup failed");
            result = -1;
        }
    }
    if (result == 0) {
        liot_trace("[tirtc] init 6/6: TiRTC home screen");
        tirtc_ui_init();
    }
    s_start_result = result;
    (void)liot_rtos_semaphore_release(s_ready_sem);
    if (result != 0) {
        if (s_tick_timer != NULL) (void)liot_rtos_timer_stop(s_tick_timer);
        if (s_touch != NULL) (void)liot_tp_deinit(s_touch);
        liot_trace("[tirtc] UI startup failed; inspect LCD/touch logs");
        (void)liot_rtos_task_delete(NULL);
        return;
    }
    /* UI init can request a different initial backlight state. */
    applied_brightness = 255U;
    heartbeat_tick = lv_tick_get();
    s_video_stats.tick = heartbeat_tick;
#if TIRTC_VIDEO_ACTIVE_GUI_PRIORITY_ENABLE
    bool active_video_priority = false;
#endif
    tirtc_ui_video_stats(&s_video_stats.previous);
    liot_trace("[tirtc] UI ready: 320x240 hardware landscape / ST7789 RGB565 + FT6336");
    while (1) {
        uint8_t requested = s_requested_brightness;
        if (requested != applied_brightness) {
            if (liot_lcd_set_brightness(s_lcd, requested) == LIOT_LCD_OK) {
                applied_brightness = requested;
            }
        }
        /* All LVGL object operations, including business updates, run here. */
        uint32_t started = lv_tick_get();
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
        tirtc_video_perf_scope_t process_scope=tirtc_video_perf_begin(TIRTC_VPERF_PROCESS);
#endif
        tirtc_ui_process();
#if TIRTC_VIDEO_ACTIVE_GUI_PRIORITY_ENABLE
        bool visible_video = tirtc_ui_connected_video_visible();
        if (visible_video != active_video_priority) {
            /* Match the other media workers while the connected video is
             * visible. F9D_D's priority API always writes the old value. */
            uint8_t old_priority;
#if TIRTC_F9_AV_FOCUS
            uint8_t priority = visible_video ? 21U : PORT_TASK_PRIORITY;
#else
            uint8_t priority = visible_video ? 11U : PORT_TASK_PRIORITY;
#endif
            if (liot_rtos_task_change_priority(s_ui_task, priority, &old_priority) == LIOT_OSI_SUCCESS)
                active_video_priority = visible_video;
        }
#endif
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
        tirtc_video_perf_end(process_scope);
#endif
        port_elapsed(&s_video_stats.process, started);
        started = lv_tick_get();
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
        tirtc_video_perf_scope_t timer_scope=tirtc_video_perf_begin(TIRTC_VPERF_TIMER);
#endif
        (void)lv_timer_handler();
#if TIRTC_VIDEO_PERF_DIAGNOSTICS
        tirtc_video_perf_end(timer_scope);
#endif
        port_elapsed(&s_video_stats.handler, started);
        ++s_video_stats.loops;
        port_video_report();
        if ((uint32_t)(lv_tick_get() - heartbeat_tick) >= 30000U) {
            heartbeat_tick = lv_tick_get();
            TIRTC_LOG_DEBUG("[tirtc] alive: uptime=%lu s page=%u", (unsigned long)(heartbeat_tick / 1000U),
                       (unsigned)tirtc_ui_current_page());
        }
        liot_rtos_task_sleep_ms(PORT_HANDLER_MS);
    }
}

int tirtc_port_start(void)
{
    if (s_ui_task != NULL) return s_start_result;
    liot_trace("[tirtc] startup: creating UI task and waiting for ready");
    /* The UI mailbox is ready before the LVGL task calls tirtc_ui_init(). */
    if (s_state_mutex == NULL &&
        liot_rtos_mutex_create(&s_state_mutex) != LIOT_OSI_SUCCESS) return -1;
    /* F6D_A liot_rtos_semaphore_delete first takes a token with an infinite
     * wait. The ready token is consumed below, so deleting this empty semaphore
     * would permanently block the app startup task. Keep this one-shot handle
     * for the application's lifetime, and reuse it if task creation fails. */
    if (s_ready_sem == NULL &&
        liot_rtos_semaphore_create(&s_ready_sem, 0U) != LIOT_OSI_SUCCESS) return -1;
    if (liot_rtos_task_create(&s_ui_task, PORT_TASK_STACK, PORT_TASK_PRIORITY,
                              "tirtc_ui", port_ui_task, NULL) != LIOT_OSI_SUCCESS) {
        return -1;
    }
    if (liot_rtos_semaphore_wait(s_ready_sem, LIOT_WAIT_FOREVER) != LIOT_OSI_SUCCESS) return -1;
    liot_trace("[tirtc] startup: ready received result=%d; F6D ready token consumed, semaphore retained", s_start_result);
    return s_start_result;
}
