/*
 * tedium-bench - measure what the I/O layer actually achieves.
 *
 * The review that motivated this rewrite made several quantitative claims:
 * that oversampling buys resolution, that scan cost bounds the usable scan
 * rate, and that CV noise is what limits 1V/oct tracking. None of those
 * should be taken on faith. This tool measures them on the real board.
 *
 * Reported:
 *   - achieved scan rate and per-scan cost, so you can pick a scan rate
 *   - CV noise floor per channel, in codes and in cents of 1V/oct
 *   - effective resolution, and the gain actually delivered by averaging
 *
 * Patch a stable DC voltage (or nothing, leaving the input at its resting
 * value) into the channels before running the noise test. A moving input
 * will be reported as noise, because from here it is indistinguishable.
 */

#include "tedium/tedium.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSAMP 20000

static void nap_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

static double stddev(const double *x, int n, double *mean_out)
{
    double m = 0.0, s = 0.0;
    int i;
    if (n < 2) return 0.0;
    for (i = 0; i < n; i++) m += x[i];
    m /= (double)n;
    for (i = 0; i < n; i++) { double d = x[i] - m; s += d * d; }
    if (mean_out) *mean_out = m;
    return sqrt(s / (double)(n - 1));
}

static void bench_timing(tt_ctx *ctx)
{
    tt_stats a, b;
    uint64_t t0, dt;
    double achieved;

    printf("scan timing\n");
    printf("-----------\n");

    tt_reset_stats(ctx);
    t0 = tt_now_ns();
    nap_ms(2000.0);
    dt = tt_now_ns() - t0;
    tt_get_stats(ctx, &b);
    (void)a;

    achieved = (double)b.scans / ((double)dt / 1e9);

    printf("  requested        %d Hz\n", tt_scan_rate(ctx));
    printf("  achieved         %.0f Hz (%.1f%% of requested)\n",
           achieved, 100.0 * achieved / (double)tt_scan_rate(ctx));
    printf("  scan cost        mean %.1f us, max %.1f us\n",
           b.scan_us_mean, b.scan_us_max);
    printf("  overruns         %llu of %llu scans\n",
           (unsigned long long)b.scan_overruns, (unsigned long long)b.scans);

    /* The scan cost is what bounds the rate: at 100 percent duty the thread
     * is doing nothing but syscalls. */
    if (b.scan_us_mean > 0.0) {
        double duty = b.scan_us_mean * achieved / 10000.0;
        /* Two separate ceilings: the syscall cost of a scan, and the
         * MCP3208 itself, which manages 100 ksps at 5 V and about half that
         * at 2.7 V. Report whichever binds first. */
        double by_cpu = 1e6 / b.scan_us_mean;
        double by_adc = 100000.0 / 6.0;   /* one full 6-channel scan */
        printf("  duty cycle       %.1f%% of one core\n", duty);
        printf("  headroom         about %.0f Hz (%s-bound)\n",
               by_cpu < by_adc ? by_cpu : by_adc,
               by_cpu < by_adc ? "cpu" : "converter");
    }
    if (b.scan_overruns > b.scans / 20)
        printf("  NOTE: many overruns. Lower the scan rate, or run with "
               "rt_priority set on a PREEMPT_RT kernel.\n");
    printf("\n");
}

static void bench_noise(tt_ctx *ctx)
{
    static double s[TT_MAX_CV][NSAMP];
    uint16_t raw[TT_MAX_CV];
    int ncv = tt_num_cv(ctx);
    int n = 0, ch, i;

    printf("CV noise floor (%d samples per channel)\n", NSAMP);
    printf("--------------------------------------\n");
    printf("patch a stable DC voltage before trusting these numbers\n\n");

    while (n < NSAMP) {
        if (tt_cv_raw(ctx, raw, TT_MAX_CV) == 0) {
            for (ch = 0; ch < ncv; ch++) s[ch][n] = (double)raw[ch];
            n++;
        }
        /* Sample slower than the scan rate so successive reads are genuinely
         * different scans rather than the same one read twice. */
        nap_ms(0.5);
    }

    printf("  ch     mean     sd(LSB)   p-p    eff.bits   1V/oct jitter\n");
    for (ch = 0; ch < ncv; ch++) {
        double mean = 0.0, sd, lo = 1e9, hi = -1e9, eff, cents;

        sd = stddev(s[ch], n, &mean);
        for (i = 0; i < n; i++) {
            if (s[ch][i] < lo) lo = s[ch][i];
            if (s[ch][i] > hi) hi = s[ch][i];
        }

        /* One code is 10V/4096; a semitone is 83.3 mV, so a code is 1.76
         * cents. Report the noise the same way a musician would hear it. */
        cents = sd * 1.7578125;
        eff   = (sd > 0.0) ? log2(4096.0 / sd) : 12.0;

        printf("  cv%-2d  %7.1f   %6.2f   %5.0f   %6.2f     %.1f cents rms\n",
               ch + 1, mean, sd, hi - lo, eff, cents);
    }

    /* Does averaging actually buy resolution? It only does if the noise is
     * white and larger than one code; if the input is quieter than 1 LSB
     * there is nothing to dither against and averaging gains almost nothing.
     * Measure rather than assume. */
    printf("\n  averaging gain (channel 1)\n");
    {
        static double avg[NSAMP];
        int factors[4] = { 2, 4, 8, 16 };
        double base = stddev(s[0], n, NULL);
        int fi;

        printf("    1x   sd %.3f LSB  (baseline)\n", base);
        for (fi = 0; fi < 4; fi++) {
            int f = factors[fi], m = 0;
            double sd, ideal;
            for (i = 0; i + f <= n; i += f) {
                double acc = 0.0;
                int k;
                for (k = 0; k < f; k++) acc += s[0][i + k];
                avg[m++] = acc / (double)f;
            }
            sd = stddev(avg, m, NULL);
            ideal = base / sqrt((double)f);
            printf("    %2dx  sd %.3f LSB  (ideal %.3f, %+.1f%% vs ideal, "
                   "%+.2f bits)\n",
                   f, sd, ideal,
                   ideal > 0.0 ? 100.0 * (sd - ideal) / ideal : 0.0,
                   base > 0.0 && sd > 0.0 ? log2(base / sd) : 0.0);
        }
        printf("    a result far worse than ideal means the noise is "
               "correlated\n    (mains hum, supply ripple) and averaging "
               "will not help much\n");
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    tt_config cfg;
    tt_ctx *ctx;
    char err[TT_ERRLEN];
    tt_board board = TT_BOARD_WM8731;
    int scan_hz = 0, force_sim = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "pcm5102a")) board = TT_BOARD_PCM5102A;
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            scan_hz = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-s")) {
            force_sim = 1;
        } else {
            fprintf(stderr,
                    "usage: %s [-b wm8731|pcm5102a] [-r scan_hz] [-s]\n",
                    argv[0]);
            return 2;
        }
    }

    tt_config_init(&cfg, board);
    if (force_sim) cfg.hal = TT_HAL_SIM;
    if (scan_hz > 0) cfg.scan_rate_hz = scan_hz;

    ctx = tt_open(&cfg, err, sizeof(err));
    if (!ctx) { fprintf(stderr, "tedium: %s\n", err); return 1; }

    printf("\nterminal tedium bench -- %s board, %s backend\n\n",
           tt_board_name(board), tt_hal_name(ctx));

    if (!strcmp(tt_hal_name(ctx), "sim"))
        printf("NOTE: simulation backend. The noise figures below are "
               "meaningless;\n      run this on the module to get real "
               "numbers.\n\n");

    bench_timing(ctx);
    bench_noise(ctx);

    tt_close(ctx);
    return 0;
}
