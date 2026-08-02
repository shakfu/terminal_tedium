/*
 * End-to-end tests against the simulation backend. These drive the real
 * sampling thread, the real ring and the real interpolator; only the SPI and
 * GPIO syscalls are replaced.
 */

#include "tt_test.h"
#include "tedium/tedium.h"

#include <time.h>

#define CV_EPS 0.01f    /* one 12-bit code is 2.4 mV, so 10 mV is generous */

static void nap_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

static tt_ctx *open_sim(tt_board board)
{
    tt_config cfg;
    char err[TT_ERRLEN];
    tt_ctx *ctx;

    tt_config_init(&cfg, board);
    cfg.hal = TT_HAL_SIM;
    /* No calibration file, so the tests see the nominal transfer regardless
     * of what happens to be installed on the developer's machine. */
    cfg.cal_path = "/nonexistent/tedium-tests/cal.json";

    ctx = tt_open(&cfg, err, sizeof(err));
    if (!ctx) TT_FAIL("tt_open failed: %s", err);
    return ctx;
}

/*
 * tt_cv_block looks *backwards* from the time it is given, so after changing
 * a simulated input a test must let the whole lookback window fill with
 * post-change scans before rendering. Waiting only for "a few scans" is not
 * enough when the scan rate is depressed, as it is under a sanitizer.
 */
static void settle(tt_ctx *ctx, double window_ms);

/* Spin until the sampling thread has published a scan taken after now. */
static void wait_for_fresh_scan(tt_ctx *ctx)
{
    uint64_t deadline = tt_now_ns() + 500000000ull;   /* 500 ms */
    tt_stats s0, s1;

    tt_get_stats(ctx, &s0);
    for (;;) {
        tt_get_stats(ctx, &s1);
        if (s1.scans >= s0.scans + 3) return;
        if (tt_now_ns() > deadline) TT_FAIL("sampling thread produced no scans");
        nap_ms(1.0);
    }
}

static void settle(tt_ctx *ctx, double window_ms)
{
    nap_ms(window_ms);
    wait_for_fresh_scan(ctx);
}

TT_TEST(core, open_and_close)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    TT_ASSERT_EQ_INT(6, tt_num_cv(ctx));
    TT_ASSERT_EQ_INT(4000, tt_scan_rate(ctx));
    TT_ASSERT_STR_EQ("sim", tt_hal_name(ctx));
    tt_close(ctx);
}

TT_TEST(core, close_tolerates_null)
{
    tt_close(NULL);
}

TT_TEST(core, pcm5102a_exposes_eight_channels)
{
    tt_ctx *ctx = open_sim(TT_BOARD_PCM5102A);
    TT_ASSERT_EQ_INT(8, tt_num_cv(ctx));
    tt_close(ctx);
}

TT_TEST(core, sampling_thread_runs_at_the_requested_rate)
{
    tt_config cfg;
    char err[TT_ERRLEN];
    tt_ctx *ctx;
    tt_stats s;
    uint64_t t0, elapsed;
    double measured;

    tt_config_init(&cfg, TT_BOARD_WM8731);
    cfg.hal = TT_HAL_SIM;
    cfg.scan_rate_hz = 2000;
    cfg.cal_path = "/nonexistent/tedium-tests/cal.json";

    ctx = tt_open(&cfg, err, sizeof(err));
    TT_ASSERT_NOT_NULL(ctx);

    tt_reset_stats(ctx);
    t0 = tt_now_ns();
    nap_ms(200.0);
    elapsed = tt_now_ns() - t0;
    tt_get_stats(ctx, &s);

    measured = (double)s.scans / ((double)elapsed / 1e9);

    /* A general purpose OS will not hit the rate exactly; a factor of two
     * either way would mean the loop is broken rather than merely jittery. */
    TT_ASSERT(measured > 1000.0);
    TT_ASSERT(measured < 4000.0);
    tt_close(ctx);
}

TT_TEST(core, default_cv_reads_as_zero_volts)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    float v[TT_MAX_CV];
    int i;

    wait_for_fresh_scan(ctx);
    TT_ASSERT_EQ_INT(0, tt_cv_latest(ctx, v, TT_MAX_CV));
    for (i = 0; i < 6; i++) TT_ASSERT_NEAR(0.0f, v[i], CV_EPS);
    tt_close(ctx);
}

TT_TEST(core, cv_roundtrips_through_calibration)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    float v[TT_MAX_CV];

    tt_sim_set_cv(ctx, 0, 2.5f);
    tt_sim_set_cv(ctx, 1, -2.5f);
    tt_sim_set_cv(ctx, 2, 0.0f);
    tt_sim_set_cv(ctx, 3, 4.9f);
    wait_for_fresh_scan(ctx);

    TT_ASSERT_EQ_INT(0, tt_cv_latest(ctx, v, TT_MAX_CV));
    TT_ASSERT_NEAR(2.5f, v[0], CV_EPS);
    TT_ASSERT_NEAR(-2.5f, v[1], CV_EPS);
    TT_ASSERT_NEAR(0.0f, v[2], CV_EPS);
    TT_ASSERT_NEAR(4.9f, v[3], CV_EPS);
    tt_close(ctx);
}

TT_TEST(core, channels_are_independent)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    float v[TT_MAX_CV];
    int i;

    /* A wrong panel map would show up here as crosstalk between jacks. */
    for (i = 0; i < 6; i++) tt_sim_set_cv(ctx, i, (float)i - 2.0f);
    wait_for_fresh_scan(ctx);

    TT_ASSERT_EQ_INT(0, tt_cv_latest(ctx, v, TT_MAX_CV));
    for (i = 0; i < 6; i++) TT_ASSERT_NEAR((float)i - 2.0f, v[i], CV_EPS);
    tt_close(ctx);
}

TT_TEST(core, panel_mapping_is_applied_on_pcm5102a)
{
    tt_ctx *ctx = open_sim(TT_BOARD_PCM5102A);
    float v[TT_MAX_CV];
    int i;

    for (i = 0; i < 8; i++) tt_sim_set_cv(ctx, i, (float)i * 0.5f - 2.0f);
    wait_for_fresh_scan(ctx);

    TT_ASSERT_EQ_INT(0, tt_cv_latest(ctx, v, TT_MAX_CV));
    for (i = 0; i < 8; i++)
        TT_ASSERT_NEAR((float)i * 0.5f - 2.0f, v[i], CV_EPS);
    tt_close(ctx);
}

TT_TEST(core, raw_codes_are_readable)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    uint16_t raw[TT_MAX_CV];

    tt_sim_set_cv(ctx, 0, 0.0f);
    wait_for_fresh_scan(ctx);

    TT_ASSERT_EQ_INT(0, tt_cv_raw(ctx, raw, TT_MAX_CV));
    TT_ASSERT_EQ_INT(2048, raw[0]);
    tt_close(ctx);
}

TT_TEST(core, cv_block_fills_every_frame)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    float buf[6][64];
    float *ptr[6];
    int ch;
    uint32_t f;

    for (ch = 0; ch < 6; ch++) {
        ptr[ch] = buf[ch];
        for (f = 0; f < 64; f++) buf[ch][f] = -999.0f;
    }

    tt_sim_set_cv(ctx, 0, 1.25f);
    settle(ctx, 20.0);

    /* Ask for a block ending at "now" so every frame is inside the ring. */
    {
        double ns_per_frame = 1e9 / 48000.0;
        uint64_t t0 = tt_now_ns() - (uint64_t)(64.0 * ns_per_frame);
        TT_ASSERT_EQ_INT(0, tt_cv_block(ctx, t0, ns_per_frame, 64, ptr, 6));
    }

    for (f = 0; f < 64; f++) TT_ASSERT_NEAR(1.25f, buf[0][f], CV_EPS);
    tt_close(ctx);
}

TT_TEST(core, cv_block_zeroes_channels_the_board_lacks)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);   /* 6 channels */
    float buf[8][32];
    float *ptr[8];
    int ch;
    uint32_t f;

    for (ch = 0; ch < 8; ch++) {
        ptr[ch] = buf[ch];
        for (f = 0; f < 32; f++) buf[ch][f] = -999.0f;
    }
    settle(ctx, 20.0);
    {
        double ns_per_frame = 1e9 / 48000.0;
        uint64_t t0 = tt_now_ns() - (uint64_t)(32.0 * ns_per_frame);
        tt_cv_block(ctx, t0, ns_per_frame, 32, ptr, 8);
    }

    for (f = 0; f < 32; f++) {
        TT_ASSERT_NEAR(0.0f, buf[6][f], 1e-6f);
        TT_ASSERT_NEAR(0.0f, buf[7][f], 1e-6f);
    }
    tt_close(ctx);
}

TT_TEST(core, cv_block_honours_null_channel_pointers)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    float buf[32];
    float *ptr[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    double ns_per_frame = 1e9 / 48000.0;
    uint64_t t0;

    ptr[2] = buf;
    tt_sim_set_cv(ctx, 2, -1.5f);
    settle(ctx, 20.0);

    t0 = tt_now_ns() - (uint64_t)(32.0 * ns_per_frame);
    TT_ASSERT_EQ_INT(0, tt_cv_block(ctx, t0, ns_per_frame, 32, ptr, 6));
    TT_ASSERT_NEAR(-1.5f, buf[16], CV_EPS);
    tt_close(ctx);
}

/*
 * The whole point of the rework: CV must come out as a continuous signal, not
 * as the staircase the legacy control-rate path produced.
 *
 * Sweep a channel in discrete steps, then render one long block spanning the
 * sweep. A control-rate hold would yield roughly as many distinct output
 * values as there were steps. Interpolation across the scan history yields
 * hundreds, with no step larger than one scan's worth of change.
 */
TT_TEST(core, cv_block_interpolates_rather_than_holding)
{
    enum { NSTEP = 30, MAXF = 2048 };
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    double ns_per_frame = 1e9 / 48000.0;
    static float buf[MAXF];
    float *ptr[1];
    uint64_t t_start, t_end;
    uint32_t nf, f;
    int step, distinct = 0;

    ptr[0] = buf;
    tt_sim_set_cv(ctx, 0, -5.0f);
    wait_for_fresh_scan(ctx);

    /* Roughly 30 ms of sweep, which at 4 kHz is about 120 scans. */
    t_start = tt_now_ns();
    for (step = 0; step <= NSTEP; step++) {
        tt_sim_set_cv(ctx, 0, -5.0f + 10.0f * (float)step / (float)NSTEP);
        nap_ms(1.0);
    }
    t_end = tt_now_ns();

    /* Render exactly the window the sweep occupied. */
    nf = (uint32_t)((double)(t_end - t_start) / ns_per_frame);
    if (nf > MAXF) nf = MAXF;
    TT_ASSERT(nf > 512);
    TT_ASSERT_EQ_INT(0, tt_cv_block(ctx, t_start, ns_per_frame, nf, ptr, 1));

    for (f = 1; f < nf; f++) {
        if (buf[f] != buf[f - 1]) distinct++;
        /* One step is 0.33 V, spread by interpolation over a 250 us scan
         * gap. No single frame may jump the whole step. */
        TT_ASSERT(fabs(buf[f] - buf[f - 1]) < 0.2);
        /* The sweep is monotonic, so the output must be too. */
        TT_ASSERT(buf[f] >= buf[f - 1] - 1e-4f);
    }

    /* A staircase would change value about NSTEP times. */
    TT_ASSERT(distinct > 4 * NSTEP);

    /* And it must actually traverse the range. */
    TT_ASSERT(buf[0] < -4.0f);
    TT_ASSERT(buf[nf - 1] > 4.0f);
    tt_close(ctx);
}

TT_TEST(core, smoothing_reduces_step_response)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    double ns_per_frame = 1e9 / 48000.0;
    float buf[64];
    float *ptr[1];
    uint64_t t0;

    ptr[0] = buf;
    tt_sim_set_cv(ctx, 0, 0.0f);
    settle(ctx, 20.0);

    /* Prime the filter at 0 V. */
    tt_set_cv_smoothing(ctx, 20.0f);
    t0 = tt_now_ns() - (uint64_t)(64.0 * ns_per_frame);
    tt_cv_block(ctx, t0, ns_per_frame, 64, ptr, 1);

    /* Now step to 5 V. A 20 Hz one-pole must not follow it within 1.3 ms. */
    tt_sim_set_cv(ctx, 0, 5.0f);
    settle(ctx, 20.0);
    t0 = tt_now_ns() - (uint64_t)(64.0 * ns_per_frame);
    tt_cv_block(ctx, t0, ns_per_frame, 64, ptr, 1);

    TT_ASSERT(buf[63] < 2.0f);

    /* Without smoothing the same step arrives in full. */
    tt_set_cv_smoothing(ctx, 0.0f);
    settle(ctx, 20.0);
    t0 = tt_now_ns() - (uint64_t)(64.0 * ns_per_frame);
    tt_cv_block(ctx, t0, ns_per_frame, 64, ptr, 1);
    TT_ASSERT_NEAR(5.0f, buf[63], 0.05f);

    tt_close(ctx);
}

TT_TEST(core, cv_block_rejects_bad_arguments)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    float buf[8];
    float *ptr[1];
    ptr[0] = buf;

    TT_ASSERT_EQ_INT(-1, tt_cv_block(ctx, 0, 1.0, 0, ptr, 1));
    TT_ASSERT_EQ_INT(-1, tt_cv_block(ctx, 0, 1.0, 8, ptr, 0));
    TT_ASSERT_EQ_INT(-1, tt_cv_block(ctx, 0, 1.0, 8, NULL, 1));
    TT_ASSERT_EQ_INT(-1, tt_cv_block(NULL, 0, 1.0, 8, ptr, 1));
    tt_close(ctx);
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

static int drain_until(tt_ctx *ctx, tt_event *out, int want, double timeout_ms)
{
    uint64_t deadline = tt_now_ns() + (uint64_t)(timeout_ms * 1e6);
    int got = 0;
    while (got < want && tt_now_ns() < deadline) {
        got += tt_poll_events(ctx, out + got, want - got);
        if (got < want) nap_ms(1.0);
    }
    return got;
}

TT_TEST(core, trigger_events_are_delivered)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    tt_event ev[4];
    int n;

    TT_ASSERT_EQ_INT(0, tt_sim_event(ctx, TT_EV_TRIGGER, 1, 1));
    TT_ASSERT_EQ_INT(0, tt_sim_event(ctx, TT_EV_TRIGGER, 1, 0));

    n = drain_until(ctx, ev, 2, 500.0);
    TT_ASSERT_EQ_INT(2, n);
    TT_ASSERT_EQ_INT(TT_EV_TRIGGER, ev[0].type);
    TT_ASSERT_EQ_INT(1, ev[0].index);
    TT_ASSERT_EQ_INT(1, ev[0].value);
    TT_ASSERT_EQ_INT(0, ev[1].value);
    TT_ASSERT(ev[1].time_ns >= ev[0].time_ns);
    tt_close(ctx);
}

TT_TEST(core, trigger_level_tracks_events)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    tt_event ev[2];

    TT_ASSERT_EQ_INT(0, tt_trigger_state(ctx, 2));
    tt_sim_event(ctx, TT_EV_TRIGGER, 2, 1);
    drain_until(ctx, ev, 1, 500.0);
    TT_ASSERT_EQ_INT(1, tt_trigger_state(ctx, 2));

    tt_sim_event(ctx, TT_EV_TRIGGER, 2, 0);
    drain_until(ctx, ev, 1, 500.0);
    TT_ASSERT_EQ_INT(0, tt_trigger_state(ctx, 2));

    /* Out of range indices must not read as active. */
    TT_ASSERT_EQ_INT(0, tt_trigger_state(ctx, 99));
    TT_ASSERT_EQ_INT(0, tt_trigger_state(ctx, -1));
    tt_close(ctx);
}

TT_TEST(core, button_hold_time_is_measured)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    tt_event ev[2];
    double held;

    TT_ASSERT_NEAR(0.0, tt_button_held_ms(ctx, 0), 1e-9);

    tt_sim_event(ctx, TT_EV_BUTTON, 0, 1);
    drain_until(ctx, ev, 1, 500.0);
    TT_ASSERT_EQ_INT(1, tt_button_state(ctx, 0));

    nap_ms(50.0);
    held = tt_button_held_ms(ctx, 0);

    /* The legacy tedium_switch counted DSP blocks and called them
     * milliseconds, reading about 25 percent short. This is real time. */
    TT_ASSERT(held >= 45.0);
    TT_ASSERT(held < 200.0);

    tt_sim_event(ctx, TT_EV_BUTTON, 0, 0);
    drain_until(ctx, ev, 1, 500.0);
    TT_ASSERT_EQ_INT(0, tt_button_state(ctx, 0));
    TT_ASSERT_NEAR(0.0, tt_button_held_ms(ctx, 0), 1e-9);
    tt_close(ctx);
}

TT_TEST(core, event_burst_is_not_reordered)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    tt_event ev[32];
    int i, n;

    for (i = 0; i < 16; i++)
        tt_sim_event(ctx, TT_EV_TRIGGER, i % 4, i & 1);

    n = drain_until(ctx, ev, 16, 1000.0);
    TT_ASSERT_EQ_INT(16, n);
    for (i = 0; i < 16; i++) {
        TT_ASSERT_EQ_INT(i % 4, ev[i].index);
        TT_ASSERT_EQ_INT(i & 1, ev[i].value);
    }
    tt_close(ctx);
}

/* ------------------------------------------------------------------ */
/* gates                                                               */
/* ------------------------------------------------------------------ */

static int wait_gate(tt_ctx *ctx, int idx, int want, double timeout_ms)
{
    uint64_t deadline = tt_now_ns() + (uint64_t)(timeout_ms * 1e6);
    while (tt_now_ns() < deadline) {
        if (tt_sim_gate_state(ctx, idx) == want) return 1;
        nap_ms(0.5);
    }
    return 0;
}

TT_TEST(core, gate_set_reaches_the_hardware)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);

    TT_ASSERT_EQ_INT(0, tt_sim_gate_state(ctx, 0));
    TT_ASSERT_EQ_INT(0, tt_gate_set(ctx, 0, 1));
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 0, 1, 200.0));

    TT_ASSERT_EQ_INT(0, tt_gate_set(ctx, 0, 0));
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 0, 0, 200.0));
    tt_close(ctx);
}

TT_TEST(core, gates_are_independent)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);

    tt_gate_set(ctx, 1, 1);
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 1, 1, 200.0));
    TT_ASSERT_EQ_INT(0, tt_sim_gate_state(ctx, 0));
    tt_close(ctx);
}

TT_TEST(core, gate_rejects_out_of_range_index)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    TT_ASSERT_EQ_INT(-1, tt_gate_set(ctx, -1, 1));
    TT_ASSERT_EQ_INT(-1, tt_gate_set(ctx, 7, 1));
    TT_ASSERT_EQ_INT(-1, tt_gate_set_at(ctx, 7, 1, tt_now_ns()));
    tt_close(ctx);
}

TT_TEST(core, scheduled_gate_waits_for_its_time)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    uint64_t at = tt_now_ns() + 60000000ull;    /* 60 ms out */

    TT_ASSERT_EQ_INT(0, tt_gate_set_at(ctx, 0, 1, at));

    /* Must still be low well before the deadline. */
    nap_ms(20.0);
    TT_ASSERT_EQ_INT(0, tt_sim_gate_state(ctx, 0));

    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 0, 1, 300.0));
    TT_ASSERT(tt_now_ns() >= at);
    tt_close(ctx);
}

TT_TEST(core, scheduled_gate_in_the_past_fires_now)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    TT_ASSERT_EQ_INT(0, tt_gate_set_at(ctx, 0, 1, tt_now_ns() - 1000000ull));
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 0, 1, 200.0));
    tt_close(ctx);
}

TT_TEST(core, immediate_set_cancels_a_pending_schedule)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);

    tt_gate_set_at(ctx, 0, 1, tt_now_ns() + 50000000ull);
    tt_gate_set(ctx, 0, 0);          /* changed our mind */

    nap_ms(120.0);
    TT_ASSERT_EQ_INT(0, tt_sim_gate_state(ctx, 0));
    tt_close(ctx);
}

TT_TEST(core, close_drives_gates_low)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    tt_gate_set(ctx, 0, 1);
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 0, 1, 200.0));
    /* Nothing to assert after free, but ASan will catch a use-after-free and
     * the gate-low write must not touch released memory. */
    tt_close(ctx);
}

/* ------------------------------------------------------------------ */
/* calibration integration                                             */
/* ------------------------------------------------------------------ */

TT_TEST(core, calibration_changes_reported_volts)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    tt_cal cal;
    float v[TT_MAX_CV];
    uint16_t raw[TT_MAX_CV];

    tt_sim_set_cv(ctx, 0, 1.0f);
    wait_for_fresh_scan(ctx);
    TT_ASSERT_EQ_INT(0, tt_cv_raw(ctx, raw, TT_MAX_CV));

    /* Halve the scale; the same code must now report half the voltage. */
    tt_cal_get(ctx, &cal);
    cal.ch[0].scale *= 0.5f;
    tt_cal_set(ctx, &cal);

    TT_ASSERT_EQ_INT(0, tt_cv_latest(ctx, v, TT_MAX_CV));
    TT_ASSERT_NEAR(0.5f, v[0], CV_EPS);
    tt_close(ctx);
}

TT_TEST(core, stats_are_collected_and_resettable)
{
    tt_ctx *ctx = open_sim(TT_BOARD_WM8731);
    tt_stats s;

    wait_for_fresh_scan(ctx);
    tt_get_stats(ctx, &s);
    TT_ASSERT(s.scans > 0);
    TT_ASSERT(s.scan_us_mean >= 0.0);
    TT_ASSERT(s.scan_us_max >= s.scan_us_mean);

    tt_reset_stats(ctx);
    tt_get_stats(ctx, &s);
    TT_ASSERT(s.scans < 100);
    tt_close(ctx);
}
