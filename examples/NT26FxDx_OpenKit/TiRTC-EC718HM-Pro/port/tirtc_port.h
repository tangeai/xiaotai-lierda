#ifndef TIRTC_PORT_H
#define TIRTC_PORT_H

#include <stdbool.h>
#include <stdint.h>
#include "../video/tirtc_video_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native panel/touch dimensions stay portrait; MADCTL addresses it landscape. */
#define TIRTC_PHYSICAL_WIDTH 240U
#define TIRTC_PHYSICAL_HEIGHT 320U
/* Same orientation since release 04; release 35 uses panel hardware rotation. */
#define TIRTC_SCREEN_WIDTH TIRTC_PHYSICAL_HEIGHT
#define TIRTC_SCREEN_HEIGHT TIRTC_PHYSICAL_WIDTH

#define TIRTC_WX_VIDEO_VIEW_X 0
#define TIRTC_WX_VIDEO_VIEW_Y 0
#define TIRTC_WX_VIDEO_VIEW_SIZE TIRTC_F9_WX_SQUARE_SIZE
#if TIRTC_WX_VIDEO_VIEW_X + TIRTC_WX_VIDEO_VIEW_SIZE > TIRTC_SCREEN_WIDTH || \
    TIRTC_WX_VIDEO_VIEW_Y + TIRTC_WX_VIDEO_VIEW_SIZE > TIRTC_SCREEN_HEIGHT
#error "The WeChat viewport must fit the landscape panel"
#endif

/* Call once from user_main. Returns 0 only after LCD, touch and UI are ready. */
int tirtc_port_start(void);

/* Thread safe. The verified GPIO backlight supports 0=off, 1..100=on.
 * Keep this percentage API for a future PWM implementation. */
void tirtc_port_set_brightness(uint8_t percent);

#if TIRTC_VIDEO_DIRECT_VIEW_ENABLE
/* The GUI task alone sends this packed, 4-byte-aligned video viewport. */
bool tirtc_port_present_call_video(const uint16_t *pixels, bool square);
#endif

#ifdef __cplusplus
}
#endif

#endif
