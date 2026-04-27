#ifndef CST328_H
#define CST328_H

#include <stdint.h>
#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CST328_MAX_FINGERS 5U

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t pressure;
    uint8_t id;
} cst328_point_t;

typedef struct {
    uint8_t finger_count;
    cst328_point_t points[CST328_MAX_FINGERS];
} cst328_touch_t;

ErrorStatus cst328_hw_init(void);
ErrorStatus cst328_hw_read_touch(cst328_touch_t *touch);

/* Reads the chip on every IRQ edge (active-low) and prints touch info to stdout.
 * Safe to call from the main loop; returns immediately if IRQ pin is high. */
void cst328_hw_poll(void);

#ifdef __cplusplus
}
#endif

#endif
