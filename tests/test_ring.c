#include "tt_test.h"
#include "tt_ring.h"

#include <pthread.h>

static void push_n(tt_cvring *r, int n, uint64_t t_step)
{
    uint16_t raw[TT_MAX_CV];
    int i, ch;
    for (i = 0; i < n; i++) {
        for (ch = 0; ch < TT_MAX_CV; ch++) raw[ch] = (uint16_t)(i * 10 + ch);
        tt_cvring_push(r, (uint64_t)(i + 1) * t_step, raw, TT_MAX_CV);
    }
}

TT_TEST(cvring, init_rounds_to_power_of_two)
{
    tt_cvring r;
    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 100));
    TT_ASSERT_EQ_INT(128, r.size);
    TT_ASSERT_EQ_INT(127, r.mask);
    tt_cvring_free(&r);
}

TT_TEST(cvring, init_enforces_minimum)
{
    tt_cvring r;
    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 1));
    TT_ASSERT_EQ_INT(64, r.size);
    tt_cvring_free(&r);
}

TT_TEST(cvring, empty_reads_fail)
{
    tt_cvring r;
    tt_scan s;
    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 64));
    TT_ASSERT_EQ_U64(0, tt_cvring_count(&r));
    TT_ASSERT_EQ_INT(-1, tt_cvring_get(&r, 0, &s));
    tt_cvring_free(&r);
}

TT_TEST(cvring, push_then_get)
{
    tt_cvring r;
    tt_scan s;
    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 64));
    push_n(&r, 5, 1000);

    TT_ASSERT_EQ_U64(5, tt_cvring_count(&r));
    TT_ASSERT_EQ_INT(0, tt_cvring_get(&r, 0, &s));
    TT_ASSERT_EQ_U64(1000, s.t_ns);
    TT_ASSERT_EQ_INT(0, s.raw[0]);
    TT_ASSERT_EQ_INT(3, s.raw[3]);

    TT_ASSERT_EQ_INT(0, tt_cvring_get(&r, 4, &s));
    TT_ASSERT_EQ_U64(5000, s.t_ns);
    TT_ASSERT_EQ_INT(40, s.raw[0]);

    /* Not yet written. */
    TT_ASSERT_EQ_INT(-1, tt_cvring_get(&r, 5, &s));
    tt_cvring_free(&r);
}

TT_TEST(cvring, push_zeroes_unused_channels)
{
    tt_cvring r;
    tt_scan s;
    uint16_t raw[3] = { 111, 222, 333 };

    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 64));
    tt_cvring_push(&r, 42, raw, 3);
    TT_ASSERT_EQ_INT(0, tt_cvring_get(&r, 0, &s));
    TT_ASSERT_EQ_INT(111, s.raw[0]);
    TT_ASSERT_EQ_INT(333, s.raw[2]);
    TT_ASSERT_EQ_INT(0, s.raw[3]);
    TT_ASSERT_EQ_INT(0, s.raw[TT_MAX_CV - 1]);
    tt_cvring_free(&r);
}

TT_TEST(cvring, lapped_reads_are_rejected)
{
    tt_cvring r;
    tt_scan s;

    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 64));
    push_n(&r, 200, 1000);

    /* Index 0 was overwritten long ago; the reader must notice rather than
     * hand back a stale scan wearing a fresh timestamp. */
    TT_ASSERT_EQ_INT(-1, tt_cvring_get(&r, 0, &s));
    TT_ASSERT_EQ_INT(-1, tt_cvring_get(&r, 135, &s));
    TT_ASSERT_EQ_INT(0, tt_cvring_get(&r, 199, &s));
    TT_ASSERT_EQ_INT(0, tt_cvring_get(&r, 136, &s));
    tt_cvring_free(&r);
}

TT_TEST(cvring, find_locates_bracketing_scan)
{
    tt_cvring r;
    uint64_t idx;

    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 64));
    push_n(&r, 10, 1000);   /* timestamps 1000..10000 */

    /* Exactly on a scan. */
    TT_ASSERT_EQ_INT(1, tt_cvring_find(&r, 0, 10, 3000, &idx));
    TT_ASSERT_EQ_U64(2, idx);

    /* Between two scans picks the earlier one. */
    TT_ASSERT_EQ_INT(1, tt_cvring_find(&r, 0, 10, 3500, &idx));
    TT_ASSERT_EQ_U64(2, idx);

    /* Past the newest clamps to the newest. */
    TT_ASSERT_EQ_INT(1, tt_cvring_find(&r, 0, 10, 999999, &idx));
    TT_ASSERT_EQ_U64(9, idx);

    /* Before the oldest reports 0 so the caller can clamp. */
    TT_ASSERT_EQ_INT(0, tt_cvring_find(&r, 0, 10, 10, &idx));
    TT_ASSERT_EQ_U64(0, idx);

    tt_cvring_free(&r);
}

TT_TEST(cvring, find_on_empty_range)
{
    tt_cvring r;
    uint64_t idx;
    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 64));
    TT_ASSERT_EQ_INT(-1, tt_cvring_find(&r, 0, 0, 100, &idx));
    tt_cvring_free(&r);
}

/* The producer runs flat out while the consumer reads the newest scan; any
 * torn read would show up as a timestamp/payload mismatch, since the payload
 * is derived from the timestamp. */
typedef struct {
    tt_cvring       *r;
    _Atomic int     *stop;
} producer_arg;

static void *producer(void *p)
{
    producer_arg *a = (producer_arg *)p;
    uint16_t raw[TT_MAX_CV];
    uint64_t i = 1;
    while (!*a->stop) {
        int ch;
        for (ch = 0; ch < TT_MAX_CV; ch++)
            raw[ch] = (uint16_t)((i + (uint64_t)ch) & 0xfff);
        tt_cvring_push(a->r, i, raw, TT_MAX_CV);
        i++;
    }
    return NULL;
}

TT_TEST(cvring, concurrent_reads_are_never_torn)
{
    tt_cvring r;
    pthread_t th;
    /* volatile is not a synchronisation primitive; use a real atomic so the
     * only races TSan can report are ones in the code under test. */
    _Atomic int stop = 0;
    producer_arg arg;
    int reads = 0, rejects = 0, i;


    TT_ASSERT_EQ_INT(0, tt_cvring_init(&r, 256));
    arg.r = &r;
    arg.stop = &stop;
    TT_ASSERT_EQ_INT(0, pthread_create(&th, NULL, producer, &arg));

    for (i = 0; i < 200000; i++) {
        uint64_t w = tt_cvring_count(&r);
        tt_scan s;
        int ch;
        if (w == 0) continue;
        if (tt_cvring_get(&r, w - 1, &s) != 0) { rejects++; continue; }
        reads++;
        for (ch = 0; ch < TT_MAX_CV; ch++) {
            uint16_t want = (uint16_t)((s.t_ns + (uint64_t)ch) & 0xfff);
            if (s.raw[ch] != want) {
                stop = 1;
                pthread_join(th, NULL);
                TT_FAIL("torn scan at t=%llu ch=%d: want %u got %u",
                        (unsigned long long)s.t_ns, ch,
                        (unsigned)want, (unsigned)s.raw[ch]);
            }
        }
    }

    stop = 1;
    pthread_join(th, NULL);
    /* Rejections are legitimate -- they mean the lap check fired. */
    TT_ASSERT(reads > 1000);
    TT_ASSERT(reads + rejects > 1000);
    tt_cvring_free(&r);
}

/* ------------------------------------------------------------------ */

TT_TEST(evring, push_pop_order)
{
    tt_evring r;
    tt_event e;
    int i;

    TT_ASSERT_EQ_INT(0, tt_evring_init(&r, 16));
    TT_ASSERT_EQ_INT(0, tt_evring_pop(&r, &e));

    for (i = 0; i < 5; i++) {
        tt_event x;
        memset(&x, 0, sizeof(x));
        x.time_ns = (uint64_t)(i + 1);
        x.index = (uint8_t)i;
        TT_ASSERT_EQ_INT(0, tt_evring_push(&r, &x));
    }
    for (i = 0; i < 5; i++) {
        TT_ASSERT_EQ_INT(1, tt_evring_pop(&r, &e));
        TT_ASSERT_EQ_U64(i + 1, e.time_ns);
        TT_ASSERT_EQ_INT(i, e.index);
    }
    TT_ASSERT_EQ_INT(0, tt_evring_pop(&r, &e));
    tt_evring_free(&r);
}

TT_TEST(evring, overflow_drops_and_counts)
{
    tt_evring r;
    tt_event e;
    int i, pushed = 0;

    TT_ASSERT_EQ_INT(0, tt_evring_init(&r, 16));
    memset(&e, 0, sizeof(e));

    for (i = 0; i < 20; i++)
        if (tt_evring_push(&r, &e) == 0) pushed++;

    TT_ASSERT_EQ_INT(16, pushed);
    TT_ASSERT_EQ_U64(4, tt_evring_dropped(&r));
    tt_evring_free(&r);
}

TT_TEST(evring, drain_respects_max)
{
    tt_evring r;
    tt_event buf[8], e;
    int i;

    TT_ASSERT_EQ_INT(0, tt_evring_init(&r, 16));
    memset(&e, 0, sizeof(e));
    for (i = 0; i < 10; i++) { e.index = (uint8_t)i; tt_evring_push(&r, &e); }

    TT_ASSERT_EQ_INT(4, tt_evring_drain(&r, buf, 4));
    TT_ASSERT_EQ_INT(0, buf[0].index);
    TT_ASSERT_EQ_INT(3, buf[3].index);
    TT_ASSERT_EQ_INT(6, tt_evring_drain(&r, buf, 8));
    TT_ASSERT_EQ_INT(4, buf[0].index);
    TT_ASSERT_EQ_INT(0, tt_evring_drain(&r, buf, 8));
    tt_evring_free(&r);
}

TT_TEST(evring, wraps_repeatedly)
{
    tt_evring r;
    tt_event e, got;
    int i;

    TT_ASSERT_EQ_INT(0, tt_evring_init(&r, 16));
    memset(&e, 0, sizeof(e));

    /* Push and pop far more than the capacity to exercise index wraparound. */
    for (i = 0; i < 1000; i++) {
        e.time_ns = (uint64_t)i;
        TT_ASSERT_EQ_INT(0, tt_evring_push(&r, &e));
        TT_ASSERT_EQ_INT(1, tt_evring_pop(&r, &got));
        TT_ASSERT_EQ_U64(i, got.time_ns);
    }
    TT_ASSERT_EQ_U64(0, tt_evring_dropped(&r));
    tt_evring_free(&r);
}
