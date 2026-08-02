#include "tt_test.h"
#include "tedium/tedium.h"

#include <math.h>

TT_TEST(time, now_is_monotonic_and_nonzero)
{
    uint64_t a = tt_now_ns();
    uint64_t b = tt_now_ns();
    TT_ASSERT(a > 0);
    TT_ASSERT(b >= a);
}

TT_TEST(time, version_string)
{
    TT_ASSERT_NOT_NULL(tt_version());
    TT_ASSERT(strlen(tt_version()) >= 5);
}

TT_TEST(timebase, first_tick_returns_measured_time)
{
    tt_timebase tb;
    uint64_t t;
    tt_timebase_init(&tb, 48000.0, 64, 0.0);
    t = tt_timebase_tick(&tb, 1000000, 64);
    TT_ASSERT_EQ_U64(1000000, t);
}

TT_TEST(timebase, period_estimate_starts_nominal)
{
    tt_timebase tb;
    tt_timebase_init(&tb, 48000.0, 64, 0.0);
    /* 64 frames at 48 kHz is 1.3333 ms. */
    TT_ASSERT_NEAR(1333333.3, tb.period_ns, 1.0);
}

/*
 * The point of the DLL is to reject callback jitter. Feed it a perfectly
 * regular clock corrupted by a deterministic pseudo-random wobble and check
 * that the filtered output is far closer to the ideal grid than the raw
 * input was. Without this the jitter of the audio callback would land
 * directly on every trigger placed against the timebase.
 */
TT_TEST(timebase, rejects_callback_jitter)
{
    tt_timebase tb;
    double period = 1e9 * 64.0 / 48000.0;
    uint64_t base = 1000000000ull;
    uint32_t seed = 12345u;
    double raw_err = 0.0, dll_err = 0.0;
    int i, n = 0;

    tt_timebase_init(&tb, 48000.0, 64, 0.5);

    for (i = 0; i < 4000; i++) {
        double ideal = (double)base + (double)i * period;
        double jitter;
        uint64_t measured, got;

        seed = seed * 1664525u + 1013904223u;
        /* +/- 200 us of jitter, comparable to a loaded Pi. */
        jitter = (((double)(seed >> 8) / 16777216.0) - 0.5) * 400000.0;

        measured = (uint64_t)(ideal + jitter);
        got = tt_timebase_tick(&tb, measured, 64);

        /* Skip the lock-in transient. */
        if (i > 500) {
            raw_err += fabs((double)measured - ideal);
            dll_err += fabs((double)got - ideal);
            n++;
        }
    }

    raw_err /= n;
    dll_err /= n;

    /* The raw signal averages ~100 us of error; the loop should cut that by
     * at least an order of magnitude. */
    TT_ASSERT(raw_err > 50000.0);
    TT_ASSERT(dll_err < raw_err / 10.0);
}

TT_TEST(timebase, tracks_a_drifting_clock)
{
    tt_timebase tb;
    /* Device clock running 100 ppm fast relative to the nominal rate. */
    double period = 1e9 * 64.0 / 48000.0 * (1.0 - 100e-6);
    uint64_t base = 1000000000ull;
    int i;

    tt_timebase_init(&tb, 48000.0, 64, 1.0);
    for (i = 0; i < 20000; i++)
        tt_timebase_tick(&tb, (uint64_t)((double)base + (double)i * period), 64);

    /* The loop must converge on the real period, not the nominal one. */
    TT_ASSERT_NEAR(period, tb.period_ns, period * 1e-4);
}

TT_TEST(timebase, relocks_after_a_large_gap)
{
    tt_timebase tb;
    double period = 1e9 * 64.0 / 48000.0;
    uint64_t base = 1000000000ull;
    uint64_t got;
    int i;

    tt_timebase_init(&tb, 48000.0, 64, 0.5);
    for (i = 0; i < 1000; i++)
        tt_timebase_tick(&tb, (uint64_t)((double)base + (double)i * period), 64);

    /* A one second stall, as if the process was suspended. */
    got = tt_timebase_tick(&tb, base + 1000000000ull, 64);
    TT_ASSERT_EQ_U64(base + 1000000000ull, got);

    /* And it must be usable again immediately afterwards. */
    for (i = 1; i < 200; i++) {
        uint64_t t = base + 1000000000ull + (uint64_t)((double)i * period);
        got = tt_timebase_tick(&tb, t, 64);
    }
    TT_ASSERT_NEAR(period, tb.period_ns, period * 0.01);
}

TT_TEST(timebase, handles_a_block_size_change)
{
    tt_timebase tb;
    double p64 = 1e9 * 64.0 / 48000.0;
    double p256 = 1e9 * 256.0 / 48000.0;
    uint64_t t = 1000000000ull;
    int i;

    tt_timebase_init(&tb, 48000.0, 64, 0.5);
    for (i = 0; i < 500; i++) { tt_timebase_tick(&tb, t, 64); t += (uint64_t)p64; }

    /* Pd's [block~] or a Csound ksmps change can do this mid-stream. */
    for (i = 0; i < 500; i++) { tt_timebase_tick(&tb, t, 256); t += (uint64_t)p256; }

    TT_ASSERT_NEAR(p256, tb.period_ns, p256 * 0.05);
}

TT_TEST(timebase, zero_arguments_get_defaults)
{
    tt_timebase tb;
    tt_timebase_init(&tb, 0.0, 0, 0.0);
    TT_ASSERT(tb.period_ns > 0.0);
    TT_ASSERT(tb.b > 0.0);
    TT_ASSERT(tb.c > 0.0);
}
