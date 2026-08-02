/*
 * tedium-cal - measure and store per-channel CV calibration.
 *
 * Why bother: the inputs are 12 bits over 10 V, so one code is 2.44 mV. A
 * 1V/oct semitone is 83.3 mV, which is 34 codes, so one code is about 1.76
 * cents. Divider and op-amp tolerance alone will put an uncalibrated channel
 * tens of cents out and leave channels mismatched. Pitch tracking is not
 * usable until this has been run.
 *
 * Procedure: patch a known voltage into every CV input, tell the tool what it
 * is, repeat for a second voltage, and it solves the two-point line per
 * channel. Two well separated points beat many clustered ones.
 */

#include "tedium/tedium.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AVG_MS 500.0

static void nap_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

/* Average the raw codes for AVG_MS, and report the spread so the user can see
 * whether the input is noisy enough to worry about. */
static void measure(tt_ctx *ctx, double *mean, double *spread, int ncv)
{
    uint16_t raw[TT_MAX_CV];
    double sum[TT_MAX_CV], lo[TT_MAX_CV], hi[TT_MAX_CV];
    int n = 0, i;
    uint64_t deadline;

    for (i = 0; i < ncv; i++) { sum[i] = 0.0; lo[i] = 1e9; hi[i] = -1e9; }

    deadline = tt_now_ns() + (uint64_t)(AVG_MS * 1e6);
    while (tt_now_ns() < deadline) {
        if (tt_cv_raw(ctx, raw, TT_MAX_CV) == 0) {
            for (i = 0; i < ncv; i++) {
                double v = (double)raw[i];
                sum[i] += v;
                if (v < lo[i]) lo[i] = v;
                if (v > hi[i]) hi[i] = v;
            }
            n++;
        }
        nap_ms(1.0);
    }

    for (i = 0; i < ncv; i++) {
        mean[i]   = n ? sum[i] / (double)n : 0.0;
        spread[i] = n ? hi[i] - lo[i] : 0.0;
    }
}

static double ask_volts(const char *prompt)
{
    char line[128];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) exit(1);
    return atof(line);
}

int main(int argc, char **argv)
{
    tt_config cfg;
    tt_ctx *ctx;
    tt_cal cal;
    char err[TT_ERRLEN];
    char path[512];
    tt_board board = TT_BOARD_WM8731;
    const char *out = NULL;
    double m1[TT_MAX_CV], s1[TT_MAX_CV], m2[TT_MAX_CV], s2[TT_MAX_CV];
    double v1, v2;
    int ncv, i, force_sim = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "pcm5102a")) board = TT_BOARD_PCM5102A;
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "-s")) {
            force_sim = 1;
        } else {
            fprintf(stderr, "usage: %s [-b wm8731|pcm5102a] [-o cal.json] [-s]\n",
                    argv[0]);
            return 2;
        }
    }

    tt_config_init(&cfg, board);
    if (force_sim) cfg.hal = TT_HAL_SIM;
    /* Start from nominal, not from whatever is already installed. */
    cfg.cal_path = "";

    ctx = tt_open(&cfg, err, sizeof(err));
    if (!ctx) { fprintf(stderr, "tedium: %s\n", err); return 1; }
    ncv = tt_num_cv(ctx);

    if (!out) {
        tt_cal_default_path(path, sizeof(path));
        out = path;
    }

    printf("terminal tedium calibration -- board %s, %d channels\n\n",
           tt_board_name(board), ncv);
    printf("You need a voltage source you trust, and a way to patch it to all\n"
           "%d CV inputs at once (a multiple, or one input at a time repeated).\n"
           "Two widely separated points give the best fit: -4 V and +4 V are\n"
           "good choices. Avoid the extremes, where the front end may clip.\n\n",
           ncv);

    v1 = ask_volts("Patch the FIRST voltage to every CV input.\n"
                   "  What is it, in volts? ");
    printf("  measuring");
    fflush(stdout);
    measure(ctx, m1, s1, ncv);
    printf(" done\n\n");

    v2 = ask_volts("Now patch the SECOND voltage.\n"
                   "  What is it, in volts? ");
    printf("  measuring");
    fflush(stdout);
    measure(ctx, m2, s2, ncv);
    printf(" done\n\n");

    if (v1 == v2) {
        fprintf(stderr, "the two voltages must differ\n");
        tt_close(ctx);
        return 1;
    }

    tt_cal_nominal(&cal, board);

    printf("channel   code@%.2fV   code@%.2fV     scale        offset   noise\n",
           v1, v2);
    printf("--------------------------------------------------------------\n");

    for (i = 0; i < ncv; i++) {
        double dcode = m2[i] - m1[i];
        double scale, offset, worst;

        if (dcode == 0.0) {
            printf("cv%-2d      %8.1f     %8.1f   NO RESPONSE -- check patching\n",
                   i + 1, m1[i], m2[i]);
            continue;
        }

        /* volts = (code - offset) * scale */
        scale  = (v2 - v1) / dcode;
        offset = m1[i] - v1 / scale;

        cal.ch[i].scale  = (float)scale;
        cal.ch[i].offset = (float)offset;

        worst = (s1[i] > s2[i]) ? s1[i] : s2[i];
        printf("cv%-2d      %8.1f     %8.1f   %+.7f  %8.1f   %.0f LSB\n",
               i + 1, m1[i], m2[i], scale, offset, worst);

        if (worst > 8.0)
            printf("           ^ noisy: %.0f codes is about %.0f cents of "
                   "1V/oct jitter\n", worst, worst * 1.76);
    }

    printf("\n");
    if (tt_cal_save_file(&cal, out) != 0) {
        fprintf(stderr, "could not write %s\n", out);
        tt_close(ctx);
        return 1;
    }
    printf("wrote %s\n", out);
    printf("Run tedium-monitor to check that each input now reads correctly.\n");

    tt_close(ctx);
    return 0;
}
