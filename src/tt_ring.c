#include "tt_ring.h"

#include <stdlib.h>
#include <string.h>

static uint32_t next_pow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* ------------------------------------------------------------------ */
/* CV history ring                                                     */
/* ------------------------------------------------------------------ */

int tt_cvring_init(tt_cvring *r, uint32_t scans)
{
    if (scans < 64) scans = 64;
    r->size = next_pow2(scans);
    r->mask = r->size - 1;
    r->buf = (tt_slot *)calloc(r->size, sizeof(tt_slot));
    if (!r->buf) return -1;
    atomic_store_explicit(&r->widx, 0, memory_order_relaxed);
    return 0;
}

void tt_cvring_free(tt_cvring *r)
{
    free(r->buf);
    r->buf = NULL;
    r->size = r->mask = 0;
}

void tt_cvring_push(tt_cvring *r, uint64_t t_ns, const uint16_t *raw, int nch)
{
    uint64_t w = atomic_load_explicit(&r->widx, memory_order_relaxed);
    tt_slot *s = &r->buf[w & r->mask];
    int i;

    if (nch > TT_MAX_CV) nch = TT_MAX_CV;
    atomic_store_explicit(&s->t_ns, t_ns, memory_order_relaxed);
    for (i = 0; i < nch; i++)
        atomic_store_explicit(&s->raw[i], raw[i], memory_order_relaxed);
    for (; i < TT_MAX_CV; i++)
        atomic_store_explicit(&s->raw[i], 0, memory_order_relaxed);

    /* Publish. The release pairs with the acquire in tt_cvring_get and makes
     * the slot's contents visible before the index that advertises them. */
    atomic_store_explicit(&r->widx, w + 1, memory_order_release);
}

uint64_t tt_cvring_count(const tt_cvring *r)
{
    return atomic_load_explicit(&r->widx, memory_order_acquire);
}

int tt_cvring_get(const tt_cvring *r, uint64_t idx, tt_scan *out)
{
    uint64_t w = atomic_load_explicit(&r->widx, memory_order_acquire);
    const tt_slot *s;
    int i;

    if (idx >= w) return -1;             /* not written yet */
    if (w - idx > r->size) return -1;    /* already lapped */

    s = &r->buf[idx & r->mask];
    out->t_ns = atomic_load_explicit(&s->t_ns, memory_order_relaxed);
    for (i = 0; i < TT_MAX_CV; i++)
        out->raw[i] = atomic_load_explicit(&s->raw[i], memory_order_relaxed);

    /* Re-check after the copy: if the producer lapped us while we were
     * reading, the values we just took may be a mix of two scans. */
    atomic_thread_fence(memory_order_acquire);
    w = atomic_load_explicit(&r->widx, memory_order_acquire);
    if (w - idx > r->size) return -1;

    return 0;
}

int tt_cvring_find(const tt_cvring *r, uint64_t lo, uint64_t hi,
                   uint64_t t_ns, uint64_t *idx_out)
{
    tt_scan s;

    if (hi <= lo) return -1;

    /* Fast path: the caller usually wants something at or near the newest
     * scan, and blocks advance monotonically. */
    if (tt_cvring_get(r, hi - 1, &s) == 0 && s.t_ns <= t_ns) {
        *idx_out = hi - 1;
        return 1;
    }
    if (tt_cvring_get(r, lo, &s) != 0) return -1;
    if (s.t_ns > t_ns) {
        *idx_out = lo;
        return 0;                        /* t_ns predates the ring */
    }

    /* Invariant: scan[lo].t_ns <= t_ns < scan[hi].t_ns. */
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (tt_cvring_get(r, mid, &s) != 0) return -1;
        if (s.t_ns <= t_ns) lo = mid;
        else                hi = mid;
    }

    *idx_out = lo;
    return 1;
}

/* ------------------------------------------------------------------ */
/* event queue                                                         */
/* ------------------------------------------------------------------ */

int tt_evring_init(tt_evring *r, uint32_t capacity)
{
    if (capacity < 16) capacity = 16;
    r->size = next_pow2(capacity);
    r->mask = r->size - 1;
    r->buf = (tt_event *)calloc(r->size, sizeof(tt_event));
    if (!r->buf) return -1;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&r->dropped, 0, memory_order_relaxed);
    return 0;
}

void tt_evring_free(tt_evring *r)
{
    free(r->buf);
    r->buf = NULL;
    r->size = r->mask = 0;
}

int tt_evring_push(tt_evring *r, const tt_event *ev)
{
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (h - t >= r->size) {
        atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
        return -1;
    }
    r->buf[h & r->mask] = *ev;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 0;
}

int tt_evring_pop(tt_evring *r, tt_event *ev)
{
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);

    if (t == h) return 0;
    *ev = r->buf[t & r->mask];
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 1;
}

int tt_evring_drain(tt_evring *r, tt_event *buf, int max)
{
    int n = 0;
    while (n < max && tt_evring_pop(r, &buf[n])) n++;
    return n;
}

uint64_t tt_evring_dropped(const tt_evring *r)
{
    return atomic_load_explicit(&r->dropped, memory_order_relaxed);
}
