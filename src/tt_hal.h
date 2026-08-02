/*
 * Hardware abstraction.
 *
 * Two backends exist. `linux' drives spidev and libgpiod on the Pi; `sim'
 * synthesises input so the whole library, the tools and every binding can be
 * built and tested on a development machine with no module attached. The sim
 * backend is not a stub -- the test suite drives real CV values and real edge
 * timings through it and asserts on what comes out the other side.
 *
 * All entry points except open/close are called from the library's sampling
 * thread, one at a time, and must not block.
 */

#ifndef TT_HAL_H
#define TT_HAL_H

#include <stddef.h>

#include "tedium/tedium.h"
#include "tt_board.h"

typedef struct {
    const char *name;

    int  (*open)(void **self, const tt_config *cfg, const tt_board_desc *bd,
                 char *err, size_t errlen);
    void (*close)(void *self);

    /* Read every channel into raw[], indexed by MCP3208 channel number, not
     * by panel position. Returns 0 or -1. */
    int  (*adc_scan)(void *self, uint16_t *raw, int nch);

    /* Drain queued GPIO edges, oldest first, with kernel timestamps already
     * normalised to active-high. Returns the count, or -1. */
    int  (*gpio_poll)(void *self, tt_event *buf, int max);

    /* Drive a gate output. */
    int  (*gate_set)(void *self, int index, int on);

    /* Simulation hooks; NULL on real hardware. */
    void (*sim_set_raw)(void *self, int adc_channel, uint16_t code);
    int  (*sim_event)(void *self, const tt_event *ev);
    int  (*sim_gate_state)(void *self, int index);
} tt_hal_ops;

const tt_hal_ops *tt_hal_sim(void);

/* Returns NULL on platforms without the Linux backend compiled in. */
const tt_hal_ops *tt_hal_linux(void);

#endif /* TT_HAL_H */
