/*
 * tedium-monitor - live view of every input.
 *
 * The first thing to run on a new build. Confirms the SPI device opens, the
 * GPIO lines are claimable, the panel mapping is right, and the calibration
 * in effect is sane.
 */

#include "tedium/tedium.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_quit;

static void on_signal(int sig) { (void)sig; g_quit = 1; }

static void nap_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

static void bar(float volts, char *out, int width)
{
    int mid = width / 2;
    int pos = mid + (int)((volts / 5.0f) * (float)mid);
    int i;
    if (pos < 0) pos = 0;
    if (pos >= width) pos = width - 1;
    for (i = 0; i < width; i++) out[i] = (i == mid) ? '|' : ' ';
    out[pos] = '*';
    out[width] = '\0';
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-b wm8731|pcm5102a] [-r scan_hz] [-s] [-c cal.json]\n"
        "  -b  board variant (default wm8731)\n"
        "  -r  ADC scan rate in Hz (default 4000)\n"
        "  -s  force the simulation backend\n"
        "  -c  calibration file\n", argv0);
}

int main(int argc, char **argv)
{
    tt_config cfg;
    tt_ctx *ctx;
    char err[TT_ERRLEN];
    tt_board board = TT_BOARD_WM8731;
    int force_sim = 0, scan_hz = 0, i;
    const char *cal = NULL;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "pcm5102a")) board = TT_BOARD_PCM5102A;
            else if (!strcmp(argv[i], "wm8731")) board = TT_BOARD_WM8731;
            else { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            scan_hz = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            cal = argv[++i];
        } else if (!strcmp(argv[i], "-s")) {
            force_sim = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    tt_config_init(&cfg, board);
    if (force_sim) cfg.hal = TT_HAL_SIM;
    if (scan_hz > 0) cfg.scan_rate_hz = scan_hz;
    cfg.cal_path = cal;

    ctx = tt_open(&cfg, err, sizeof(err));
    if (!ctx) {
        fprintf(stderr, "tedium: %s\n", err);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("board %s, %d CV channels, %s backend, scanning at %d Hz\n",
           tt_board_name(board), tt_num_cv(ctx), tt_hal_name(ctx),
           tt_scan_rate(ctx));
    printf("press a button or send a trigger; ctrl-c to quit\n\n");

    while (!g_quit) {
        float v[TT_MAX_CV];
        uint16_t raw[TT_MAX_CV];
        tt_event ev[32];
        tt_stats st;
        char b[65];
        int n, ch, ncv = tt_num_cv(ctx);

        tt_cv_latest(ctx, v, TT_MAX_CV);
        tt_cv_raw(ctx, raw, TT_MAX_CV);

        printf("\033[H\033[J");
        printf("terminal tedium  [%s]\n\n", tt_hal_name(ctx));

        for (ch = 0; ch < ncv; ch++) {
            bar(v[ch], b, 48);
            printf("  cv%d %+6.3f V  %4u  [%s]\n", ch + 1, (double)v[ch],
                   (unsigned)raw[ch], b);
        }

        printf("\n  triggers ");
        for (ch = 0; ch < TT_MAX_TRIGGERS; ch++)
            printf("%d:%s ", ch + 1, tt_trigger_state(ctx, ch) ? "ON " : "-  ");

        printf("\n  buttons  ");
        for (ch = 0; ch < TT_MAX_BUTTONS; ch++) {
            if (tt_button_state(ctx, ch))
                printf("%d:%.0fms ", ch + 1, tt_button_held_ms(ctx, ch));
            else
                printf("%d:-    ", ch + 1);
        }
        printf("\n");

        n = tt_poll_events(ctx, ev, 32);
        if (n > 0)
            printf("\n  %d event(s), latest: %s %d = %d\n", n,
                   ev[n - 1].type == TT_EV_TRIGGER ? "trigger" : "button",
                   ev[n - 1].index + 1, ev[n - 1].value);
        else
            printf("\n\n");

        tt_get_stats(ctx, &st);
        printf("\n  scans %llu  overruns %llu  events %llu (dropped %llu)\n",
               (unsigned long long)st.scans,
               (unsigned long long)st.scan_overruns,
               (unsigned long long)st.events,
               (unsigned long long)st.events_dropped);
        printf("  scan time  mean %.1f us  max %.1f us\n",
               st.scan_us_mean, st.scan_us_max);

        fflush(stdout);
        nap_ms(50.0);
    }

    printf("\n");
    tt_close(ctx);
    return 0;
}
