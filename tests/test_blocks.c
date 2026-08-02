/*
 * tt_trigger_block / tt_gate_block.
 *
 * These cover the central claim of the rework: that a kernel-timestamped
 * edge lands on an exact sample instead of being quantised to a DSP block.
 * The sim backend lets us inject an edge at a chosen time and assert on the
 * frame it comes out at.
 */

#include "tt_test.h"
#include "tedium/tedium.h"

#include <time.h>

#define SR          48000.0
#define NS_PER_FRAME (1e9 / SR)

static void nap_ms(double ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
}

static tt_ctx *open_sim(void)
{
    tt_config cfg;
    char err[TT_ERRLEN];
    tt_ctx *ctx;

    tt_config_init(&cfg, TT_BOARD_WM8731);
    cfg.hal = TT_HAL_SIM;
    cfg.cal_path = "/nonexistent/tedium-tests/cal.json";
    ctx = tt_open(&cfg, err, sizeof(err));
    if (!ctx) TT_FAIL("tt_open failed: %s", err);
    return ctx;
}

/* Wait until the sampling thread has picked the injected edge up. */
static void wait_for_events(tt_ctx *ctx, int n, double timeout_ms)
{
    uint64_t deadline = tt_now_ns() + (uint64_t)(timeout_ms * 1e6);
    tt_stats s;
    for (;;) {
        tt_get_stats(ctx, &s);
        if (s.events >= (uint64_t)n) return;
        if (tt_now_ns() > deadline) TT_FAIL("only %llu of %d events arrived",
                                            (unsigned long long)s.events, n);
        nap_ms(1.0);
    }
}

/* Find the first frame at which a buffer changes value. */
static int first_edge(const float *b, uint32_t n)
{
    uint32_t f;
    for (f = 1; f < n; f++)
        if (b[f] != b[f - 1]) return (int)f;
    return -1;
}

TT_TEST(trigblock, idle_output_is_low)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    float *ptr[1];
    uint32_t f;

    ptr[0] = buf;
    for (f = 0; f < 64; f++) buf[f] = -1.0f;

    TT_ASSERT_EQ_INT(0, tt_trigger_block(ctx, tt_now_ns() - 10000000ull,
                                         NS_PER_FRAME, 64, ptr, 1));
    for (f = 0; f < 64; f++) TT_ASSERT_NEAR(0.0f, buf[f], 1e-9f);
    tt_close(ctx);
}

TT_TEST(trigblock, zeroes_channels_beyond_the_board)
{
    tt_ctx *ctx = open_sim();
    float buf[8][32];
    float *ptr[8];
    int ch;
    uint32_t f;

    for (ch = 0; ch < 8; ch++) {
        ptr[ch] = buf[ch];
        for (f = 0; f < 32; f++) buf[ch][f] = 7.0f;
    }
    tt_trigger_block(ctx, tt_now_ns() - 10000000ull, NS_PER_FRAME, 32, ptr, 8);

    /* The board has 4 triggers; 5..8 must be hard zero. */
    for (ch = 4; ch < 8; ch++)
        for (f = 0; f < 32; f++) TT_ASSERT_NEAR(0.0f, buf[ch][f], 1e-9f);
    tt_close(ctx);
}

/*
 * The headline test. Inject an edge, note the time the sim stamped it, then
 * render the window containing it and check the edge lands on the frame the
 * timestamp implies -- not merely somewhere in the block.
 */
TT_TEST(trigblock, edge_lands_on_the_exact_frame)
{
    tt_ctx *ctx = open_sim();
    enum { NF = 256 };
    float buf[NF];
    float *ptr[1];
    tt_event ev[8];
    uint64_t t_edge, t0;
    int want, got, n;

    ptr[0] = buf;

    t_edge = tt_now_ns();
    TT_ASSERT_EQ_INT(0, tt_sim_event(ctx, TT_EV_TRIGGER, 0, 1));
    wait_for_events(ctx, 1, 500.0);

    /* Recover the exact timestamp the event carries. */
    n = tt_poll_events(ctx, ev, 8);
    TT_ASSERT_EQ_INT(1, n);
    t_edge = ev[0].time_ns;

    /* Render a window that starts 100 frames before the edge. */
    t0 = t_edge - (uint64_t)(100.0 * NS_PER_FRAME);
    TT_ASSERT_EQ_INT(0, tt_trigger_block(ctx, t0, NS_PER_FRAME, NF, ptr, 1));

    want = 100;
    got = first_edge(buf, NF);

    /* Allow one frame of rounding either way. A block-quantised
     * implementation would put this at frame 0 or 255. */
    TT_ASSERT(got >= want - 1);
    TT_ASSERT(got <= want + 1);
    TT_ASSERT_NEAR(0.0f, buf[0], 1e-9f);
    TT_ASSERT_NEAR(1.0f, buf[NF - 1], 1e-9f);
    tt_close(ctx);
}

TT_TEST(trigblock, level_persists_into_the_next_block)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    float *ptr[1];
    tt_event ev[8];
    uint64_t t_edge, t0;
    uint32_t f;

    ptr[0] = buf;
    tt_sim_event(ctx, TT_EV_TRIGGER, 0, 1);
    wait_for_events(ctx, 1, 500.0);
    tt_poll_events(ctx, ev, 8);
    t_edge = ev[0].time_ns;

    /* Block containing the edge. */
    t0 = t_edge - (uint64_t)(32.0 * NS_PER_FRAME);
    tt_trigger_block(ctx, t0, NS_PER_FRAME, 64, ptr, 1);
    TT_ASSERT_NEAR(1.0f, buf[63], 1e-9f);

    /* The following block must stay high with no further events. */
    t0 += (uint64_t)(64.0 * NS_PER_FRAME);
    tt_trigger_block(ctx, t0, NS_PER_FRAME, 64, ptr, 1);
    for (f = 0; f < 64; f++) TT_ASSERT_NEAR(1.0f, buf[f], 1e-9f);
    tt_close(ctx);
}

TT_TEST(trigblock, future_edges_are_held_not_dropped)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    float *ptr[1];
    tt_event ev[8];
    uint64_t t_edge, t0;
    uint32_t f;

    ptr[0] = buf;
    tt_sim_event(ctx, TT_EV_TRIGGER, 0, 1);
    wait_for_events(ctx, 1, 500.0);
    tt_poll_events(ctx, ev, 8);
    t_edge = ev[0].time_ns;

    /* Render a window that ends well before the edge. */
    t0 = t_edge - (uint64_t)(10000.0 * NS_PER_FRAME);
    tt_trigger_block(ctx, t0, NS_PER_FRAME, 64, ptr, 1);
    for (f = 0; f < 64; f++) TT_ASSERT_NEAR(0.0f, buf[f], 1e-9f);

    /* Now render the window that does contain it. The edge must not have
     * been consumed by the earlier call. */
    t0 = t_edge - (uint64_t)(32.0 * NS_PER_FRAME);
    tt_trigger_block(ctx, t0, NS_PER_FRAME, 64, ptr, 1);
    TT_ASSERT(first_edge(buf, 64) > 0);
    TT_ASSERT_NEAR(1.0f, buf[63], 1e-9f);
    tt_close(ctx);
}

TT_TEST(trigblock, late_edges_land_at_frame_zero_rather_than_vanishing)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    float *ptr[1];
    tt_event ev[8];
    uint64_t t_edge, t0;
    uint32_t f;

    ptr[0] = buf;
    tt_sim_event(ctx, TT_EV_TRIGGER, 2, 1);
    wait_for_events(ctx, 1, 500.0);
    tt_poll_events(ctx, ev, 8);
    t_edge = ev[0].time_ns;

    /* Window starts after the edge already happened. */
    t0 = t_edge + (uint64_t)(1000.0 * NS_PER_FRAME);
    ptr[0] = buf;
    {
        float *p4[4] = { NULL, NULL, buf, NULL };
        tt_trigger_block(ctx, t0, NS_PER_FRAME, 64, p4, 4);
    }
    for (f = 0; f < 64; f++) TT_ASSERT_NEAR(1.0f, buf[f], 1e-9f);
    tt_close(ctx);
}

TT_TEST(trigblock, channels_are_independent)
{
    tt_ctx *ctx = open_sim();
    float b0[128], b1[128], b2[128], b3[128];
    float *ptr[4];
    tt_event ev[8];
    uint64_t t0;
    uint32_t f;

    ptr[0] = b0; ptr[1] = b1; ptr[2] = b2; ptr[3] = b3;

    tt_sim_event(ctx, TT_EV_TRIGGER, 1, 1);
    wait_for_events(ctx, 1, 500.0);
    tt_poll_events(ctx, ev, 8);

    t0 = ev[0].time_ns - (uint64_t)(64.0 * NS_PER_FRAME);
    tt_trigger_block(ctx, t0, NS_PER_FRAME, 128, ptr, 4);

    TT_ASSERT_NEAR(1.0f, b1[127], 1e-9f);
    for (f = 0; f < 128; f++) {
        TT_ASSERT_NEAR(0.0f, b0[f], 1e-9f);
        TT_ASSERT_NEAR(0.0f, b2[f], 1e-9f);
        TT_ASSERT_NEAR(0.0f, b3[f], 1e-9f);
    }
    tt_close(ctx);
}

/*
 * A pulse shorter than one DSP block is exactly what the legacy 1 ms polling
 * loop dropped. Here both edges must survive and both must be placed.
 */
TT_TEST(trigblock, sub_block_pulse_survives)
{
    tt_ctx *ctx = open_sim();
    enum { NF = 512 };
    float buf[NF];
    float *ptr[1];
    tt_event ev[8];
    uint64_t t_on, t_off, t0;
    int on_at, off_at, n;
    uint32_t f;

    ptr[0] = buf;

    tt_sim_event(ctx, TT_EV_TRIGGER, 0, 1);
    nap_ms(2.0);
    tt_sim_event(ctx, TT_EV_TRIGGER, 0, 0);
    wait_for_events(ctx, 2, 1000.0);

    n = tt_poll_events(ctx, ev, 8);
    TT_ASSERT_EQ_INT(2, n);
    t_on  = ev[0].time_ns;
    t_off = ev[1].time_ns;
    TT_ASSERT(t_off > t_on);

    t0 = t_on - (uint64_t)(64.0 * NS_PER_FRAME);
    TT_ASSERT_EQ_INT(0, tt_trigger_block(ctx, t0, NS_PER_FRAME, NF, ptr, 1));

    on_at = first_edge(buf, NF);
    TT_ASSERT(on_at >= 63 && on_at <= 65);

    /* Find the falling edge after that. */
    off_at = -1;
    for (f = (uint32_t)on_at + 1; f < NF; f++)
        if (buf[f] != buf[f - 1]) { off_at = (int)f; break; }

    TT_ASSERT(off_at > on_at);
    TT_ASSERT_NEAR(1.0f, buf[on_at], 1e-9f);
    TT_ASSERT_NEAR(0.0f, buf[off_at], 1e-9f);

    /* The measured pulse width must match the timestamps. */
    {
        double want = (double)(t_off - t_on) / NS_PER_FRAME;
        double got  = (double)(off_at - on_at);
        TT_ASSERT_NEAR(want, got, 2.0);
    }
    tt_close(ctx);
}

TT_TEST(trigblock, rejects_bad_arguments)
{
    tt_ctx *ctx = open_sim();
    float buf[8];
    float *ptr[1];
    ptr[0] = buf;

    TT_ASSERT_EQ_INT(-1, tt_trigger_block(ctx, 0, NS_PER_FRAME, 0, ptr, 1));
    TT_ASSERT_EQ_INT(-1, tt_trigger_block(ctx, 0, NS_PER_FRAME, 8, ptr, 0));
    TT_ASSERT_EQ_INT(-1, tt_trigger_block(ctx, 0, 0.0, 8, ptr, 1));
    TT_ASSERT_EQ_INT(-1, tt_trigger_block(ctx, 0, NS_PER_FRAME, 8, NULL, 1));
    TT_ASSERT_EQ_INT(-1, tt_trigger_block(NULL, 0, NS_PER_FRAME, 8, ptr, 1));
    tt_close(ctx);
}

/* ------------------------------------------------------------------ */
/* gate output                                                         */
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

TT_TEST(gateblock, rising_signal_raises_the_gate)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    const float *ptr[1];
    uint32_t f;

    for (f = 0; f < 64; f++) buf[f] = (f < 32) ? 0.0f : 1.0f;
    ptr[0] = buf;

    TT_ASSERT_EQ_INT(0, tt_gate_block(ctx, tt_now_ns(), NS_PER_FRAME, 64,
                                      ptr, 1));
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 0, 1, 300.0));
    tt_close(ctx);
}

TT_TEST(gateblock, constant_low_leaves_the_gate_alone)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    const float *ptr[1];
    uint32_t f;

    for (f = 0; f < 64; f++) buf[f] = 0.0f;
    ptr[0] = buf;

    tt_gate_block(ctx, tt_now_ns(), NS_PER_FRAME, 64, ptr, 1);
    nap_ms(20.0);
    TT_ASSERT_EQ_INT(0, tt_sim_gate_state(ctx, 0));
    tt_close(ctx);
}

TT_TEST(gateblock, null_channel_is_skipped)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    const float *ptr[2];
    uint32_t f;

    for (f = 0; f < 64; f++) buf[f] = 1.0f;
    ptr[0] = NULL;
    ptr[1] = buf;

    TT_ASSERT_EQ_INT(0, tt_gate_block(ctx, tt_now_ns(), NS_PER_FRAME, 64,
                                      ptr, 2));
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 1, 1, 300.0));
    TT_ASSERT_EQ_INT(0, tt_sim_gate_state(ctx, 0));
    tt_close(ctx);
}

TT_TEST(gateblock, threshold_is_half)
{
    tt_ctx *ctx = open_sim();
    float buf[64];
    const float *ptr[1];
    uint32_t f;

    for (f = 0; f < 64; f++) buf[f] = 0.4f;
    ptr[0] = buf;
    tt_gate_block(ctx, tt_now_ns(), NS_PER_FRAME, 64, ptr, 1);
    nap_ms(20.0);
    TT_ASSERT_EQ_INT(0, tt_sim_gate_state(ctx, 0));

    for (f = 0; f < 64; f++) buf[f] = 0.6f;
    tt_gate_block(ctx, tt_now_ns(), NS_PER_FRAME, 64, ptr, 1);
    TT_ASSERT_EQ_INT(1, wait_gate(ctx, 0, 1, 300.0));
    tt_close(ctx);
}

TT_TEST(gateblock, rejects_bad_arguments)
{
    tt_ctx *ctx = open_sim();
    float buf[8];
    const float *ptr[1];
    ptr[0] = buf;

    TT_ASSERT_EQ_INT(-1, tt_gate_block(ctx, 0, NS_PER_FRAME, 0, ptr, 1));
    TT_ASSERT_EQ_INT(-1, tt_gate_block(ctx, 0, NS_PER_FRAME, 8, ptr, 0));
    TT_ASSERT_EQ_INT(-1, tt_gate_block(ctx, 0, NS_PER_FRAME, 8, NULL, 1));
    TT_ASSERT_EQ_INT(-1, tt_gate_block(NULL, 0, NS_PER_FRAME, 8, ptr, 1));
    tt_close(ctx);
}
