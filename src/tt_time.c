/*
 * Monotonic clock and the delay-locked loop used to recover a stable block
 * timebase on hosts that do not expose one.
 */

#include "tedium/tedium.h"

#include <math.h>
#include <time.h>

uint64_t tt_now_ns(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW) && defined(__linux__)
    /* RAW is immune to NTP slewing, which matters because we correlate these
     * timestamps with kernel GPIO edge stamps. The kernel stamps edges with
     * CLOCK_MONOTONIC, so use the same base -- not RAW -- for correlation.  */
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const char *tt_version(void)
{
    return "0.1.0";
}

/* ------------------------------------------------------------------ */
/* DLL timebase                                                        */
/* ------------------------------------------------------------------ */

/*
 * Second-order delay-locked loop, after Fons Adriaensen, "Using a DLL to
 * filter time" (2005). The audio callback arrives at a jittery time but at a
 * very stable average rate; the loop locks to the rate and rejects the
 * jitter, which is exactly what is needed before placing a GPIO edge at a
 * sample offset inside the block.
 *
 *   e   = measured - predicted        phase error
 *   t0  = predicted                   filtered block start (returned)
 *   t1 += b*e + period                predicted start of the next block
 *   period += c*e                     tracked block duration
 */

void tt_timebase_init(tt_timebase *tb, double sample_rate, uint32_t nframes,
                      double bandwidth_hz)
{
    double w;

    if (sample_rate <= 0.0) sample_rate = 48000.0;
    if (nframes == 0)       nframes = 64;
    /* The callback rate is sample_rate/nframes; a bandwidth well below that
     * gives good jitter rejection while still tracking clock drift. */
    if (bandwidth_hz <= 0.0) bandwidth_hz = 0.5;

    tb->period_ns = 1e9 * (double)nframes / sample_rate;
    tb->t_next    = 0.0;
    tb->frames    = 0;
    tb->warmed    = 0;

    w = 2.0 * M_PI * bandwidth_hz * (double)nframes / sample_rate;
    tb->b = sqrt(2.0) * w;
    tb->c = w * w;
}

uint64_t tt_timebase_tick(tt_timebase *tb, uint64_t now_ns, uint32_t nframes)
{
    double now = (double)now_ns;
    double t0, e;

    if (!tb->warmed) {
        tb->warmed = 1;
        tb->t_next = now + tb->period_ns;
        tb->frames = nframes;
        return now_ns;
    }

    /* The loop was initialised for a particular block size. A host that
     * changes it mid-stream (Pd's [block~], Csound's ksmps) invalidates the
     * period estimate, so re-seed rather than drag the loop across. */
    if (nframes != 0 && tb->frames != 0) {
        double expect = tb->period_ns;
        double ratio  = (double)nframes * tb->period_ns /
                        ((double)tb->frames * expect);
        if (ratio < 0.5 || ratio > 2.0) {
            tb->period_ns *= (double)nframes / (double)tb->frames;
            tb->t_next = now + tb->period_ns;
            tb->frames = nframes;
            return now_ns;
        }
    }
    tb->frames = nframes;

    t0 = tb->t_next;
    e  = now - t0;

    /* A gap far beyond the loop's pull-in range means the stream stalled (a
     * device reopen, a suspended process). Relocking beats crawling back. */
    if (fabs(e) > 10.0 * tb->period_ns) {
        tb->t_next = now + tb->period_ns;
        return now_ns;
    }

    tb->t_next    = t0 + tb->b * e + tb->period_ns;
    tb->period_ns = tb->period_ns + tb->c * e;

    return (uint64_t)(t0 < 0.0 ? 0.0 : t0);
}
