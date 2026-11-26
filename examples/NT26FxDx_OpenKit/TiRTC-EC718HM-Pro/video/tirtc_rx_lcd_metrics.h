#ifndef TIRTC_RX_LCD_METRICS_H
#define TIRTC_RX_LCD_METRICS_H

#include <stdint.h>

#ifndef TIRTC_RX_LCD_METRICS
#define TIRTC_RX_LCD_METRICS 0
#endif

typedef enum {
    TIRTC_RX_LCD_RX,
    TIRTC_RX_LCD_DEC,
    TIRTC_RX_LCD_PUB,
} tirtc_rx_lcd_event_t;

#if TIRTC_RX_LCD_METRICS
/* Begin and end from the same video worker. Begin precedes RX admission.
 * End closes the measurement interval and prints the completed counters. */
void tirtc_rx_lcd_begin(uint32_t generation);
void tirtc_rx_lcd_end(uint32_t generation);

/* Generation-tagged calls mark admitted compressed frames, successful
 * decodes and successful UI publications respectively. */
void tirtc_rx_lcd_record(uint32_t generation, tirtc_rx_lcd_event_t event);

/* Call exactly once per new video frame after successful LCD transfer,
 * on both the direct viewport and LVGL fallback paths. */
void tirtc_rx_lcd_transfer(void);
#else
static inline void tirtc_rx_lcd_begin(uint32_t generation) { (void)generation; }
static inline void tirtc_rx_lcd_end(uint32_t generation) { (void)generation; }
static inline void tirtc_rx_lcd_record(uint32_t generation, tirtc_rx_lcd_event_t event)
{
    (void)generation;
    (void)event;
}
static inline void tirtc_rx_lcd_transfer(void) {}
#endif

#endif
