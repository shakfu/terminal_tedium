/*
 * Simulation backend.
 *
 * Holds a settable 12-bit code per ADC channel and an injectable edge queue,
 * so tests can drive exact values and exact edge times and assert on the
 * interpolated output. Gate writes are recorded and readable back.
 *
 * Injection happens on the control thread and is consumed by the sampling
 * thread, so the edge queue is the same SPSC ring used for real events.
 */

#include "tt_hal.h"
#include "tt_ring.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const tt_board_desc *bd;
    _Atomic uint32_t     code[TT_MAX_CV];   /* by ADC channel */
    _Atomic uint32_t     gate[TT_MAX_GATES];
    tt_evring            injected;
} sim_dev;

static int sim_open(void **self, const tt_config *cfg, const tt_board_desc *bd,
                    char *err, size_t errlen)
{
    sim_dev *d;
    int i;

    (void)cfg;
    d = (sim_dev *)calloc(1, sizeof(*d));
    if (!d) {
        if (err) snprintf(err, errlen, "sim: out of memory");
        return -1;
    }
    if (tt_evring_init(&d->injected, 256) != 0) {
        free(d);
        if (err) snprintf(err, errlen, "sim: could not allocate event ring");
        return -1;
    }
    d->bd = bd;
    /* Mid-scale is 0 V under the nominal transfer, so an untouched sim reads
     * as a patched-but-centred module rather than as a rail. */
    for (i = 0; i < TT_MAX_CV; i++)
        atomic_store_explicit(&d->code[i], 2048u, memory_order_relaxed);

    *self = d;
    return 0;
}

static void sim_close(void *self)
{
    sim_dev *d = (sim_dev *)self;
    if (!d) return;
    tt_evring_free(&d->injected);
    free(d);
}

static int sim_adc_scan(void *self, uint16_t *raw, int nch)
{
    sim_dev *d = (sim_dev *)self;
    int i;
    if (nch > TT_MAX_CV) nch = TT_MAX_CV;
    for (i = 0; i < nch; i++)
        raw[i] = (uint16_t)atomic_load_explicit(&d->code[i],
                                                memory_order_relaxed);
    return 0;
}

static int sim_gpio_poll(void *self, tt_event *buf, int max)
{
    sim_dev *d = (sim_dev *)self;
    return tt_evring_drain(&d->injected, buf, max);
}

static int sim_gate_set(void *self, int index, int on)
{
    sim_dev *d = (sim_dev *)self;
    if (index < 0 || index >= TT_MAX_GATES) return -1;
    atomic_store_explicit(&d->gate[index], on ? 1u : 0u, memory_order_relaxed);
    return 0;
}

static void sim_set_raw(void *self, int adc_channel, uint16_t code)
{
    sim_dev *d = (sim_dev *)self;
    if (adc_channel < 0 || adc_channel >= TT_MAX_CV) return;
    atomic_store_explicit(&d->code[adc_channel], code, memory_order_relaxed);
}

static int sim_inject(void *self, const tt_event *ev)
{
    sim_dev *d = (sim_dev *)self;
    return tt_evring_push(&d->injected, ev);
}

static int sim_gate_state(void *self, int index)
{
    sim_dev *d = (sim_dev *)self;
    if (index < 0 || index >= TT_MAX_GATES) return -1;
    return (int)atomic_load_explicit(&d->gate[index], memory_order_relaxed);
}

static const tt_hal_ops k_sim_ops = {
    "sim",
    sim_open,
    sim_close,
    sim_adc_scan,
    sim_gpio_poll,
    sim_gate_set,
    sim_set_raw,
    sim_inject,
    sim_gate_state
};

const tt_hal_ops *tt_hal_sim(void)
{
    return &k_sim_ops;
}

#ifndef __linux__
const tt_hal_ops *tt_hal_linux(void)
{
    return NULL;
}
#endif
