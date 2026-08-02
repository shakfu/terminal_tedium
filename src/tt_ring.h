/*
 * Lock-free single-producer / single-consumer rings shared between the
 * library's sampling thread and the host's audio callback.
 *
 * Two different shapes are needed:
 *
 *   tt_cvring   a history buffer, not a queue. The consumer does not pop; it
 *               looks up the two scans bracketing an arbitrary instant so it
 *               can interpolate. The producer overwrites the oldest slot and
 *               never blocks. Correctness rests on the reader detecting that
 *               it was lapped mid-copy rather than on any mutual exclusion.
 *
 *   tt_evring   an ordinary SPSC queue for GPIO edges, which must be
 *               delivered exactly once and in order.
 */

#ifndef TT_RING_H
#define TT_RING_H

#include <stdatomic.h>
#include <stdint.h>

#include "tedium/tedium.h"

/* One complete pass over every CV channel, timestamped when the pass began.
 * This is the plain value type handed to callers. */
typedef struct {
    uint64_t t_ns;
    uint16_t raw[TT_MAX_CV];
} tt_scan;

/* ------------------------------------------------------------------ */
/* CV history ring                                                     */
/* ------------------------------------------------------------------ */

/*
 * Storage slots are atomic even though the lapping check already detects a
 * torn read. Detecting the tear afterwards makes the *result* correct, but a
 * plain overlapping read is still a data race, which is undefined behaviour
 * and makes ThreadSanitizer useless on the rest of the library. Relaxed
 * atomics compile to ordinary loads and stores on every target we care
 * about, so being well-defined here costs nothing.
 */
typedef struct {
    _Atomic uint64_t t_ns;
    _Atomic uint16_t raw[TT_MAX_CV];
} tt_slot;

typedef struct {
    tt_slot         *buf;
    uint32_t         size;   /* power of two */
    uint32_t         mask;
    _Atomic uint64_t widx;   /* total scans ever written */
} tt_cvring;

/* scans is rounded up to a power of two, minimum 64. Returns 0 or -1. */
int  tt_cvring_init(tt_cvring *r, uint32_t scans);
void tt_cvring_free(tt_cvring *r);

/* Producer. Copies nch codes; the rest of the slot is zeroed. */
void tt_cvring_push(tt_cvring *r, uint64_t t_ns, const uint16_t *raw, int nch);

/* Consumer. Total scans written so far. */
uint64_t tt_cvring_count(const tt_cvring *r);

/* Consumer. Copy the scan at absolute index idx.
 * Returns 0 on success, -1 if idx has not been written yet or was overwritten
 * while being copied. A -1 means the caller fell too far behind, which at the
 * default ring depth means it stalled for a quarter of a second. */
int tt_cvring_get(const tt_cvring *r, uint64_t idx, tt_scan *out);

/* Consumer. Find the newest scan whose timestamp is <= t_ns, searching the
 * half-open index range [lo, hi). Writes its absolute index to idx_out.
 *
 * Returns  1  a bracketing scan was found
 *          0  t_ns predates every scan still in the ring (idx_out = lo)
 *         -1  the range is empty or entirely unreadable
 */
int tt_cvring_find(const tt_cvring *r, uint64_t lo, uint64_t hi,
                   uint64_t t_ns, uint64_t *idx_out);

/* ------------------------------------------------------------------ */
/* event queue                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    tt_event        *buf;
    uint32_t         size;   /* power of two */
    uint32_t         mask;
    _Atomic uint32_t head;   /* producer */
    _Atomic uint32_t tail;   /* consumer */
    _Atomic uint64_t dropped;
} tt_evring;

int  tt_evring_init(tt_evring *r, uint32_t capacity);
void tt_evring_free(tt_evring *r);

/* Producer. Returns 0, or -1 if the queue is full (the event is dropped and
 * the drop counter incremented). */
int tt_evring_push(tt_evring *r, const tt_event *ev);

/* Consumer. Returns 1 and fills ev, or 0 when empty. */
int tt_evring_pop(tt_evring *r, tt_event *ev);

/* Consumer. Drains up to max events; returns the count. */
int tt_evring_drain(tt_evring *r, tt_event *buf, int max);

uint64_t tt_evring_dropped(const tt_evring *r);

#endif /* TT_RING_H */
