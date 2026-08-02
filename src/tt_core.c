/*
 * libtedium core: context lifecycle, the sampling thread, and the wait-free
 * reader the audio callback uses.
 *
 * The design that matters is the split between the two threads. The legacy
 * externals did their SPI transfers inside Pd's scheduler, so a `smooth 16'
 * setting put 96 blocking ioctls in the middle of the audio thread. Here the
 * sampling thread owns all the syscalls and publishes into a history ring;
 * the audio callback only ever reads memory and interpolates.
 *
 * Because the ring is a history rather than a queue, the reader can ask for
 * CV "as of" any instant within the ring's depth. That is what makes CV a
 * signal rather than a sequence of control events, and it is what lets a
 * kernel-timestamped GPIO edge land on an exact sample.
 */

#include "tedium/tedium.h"
#include "tt_board.h"
#include "tt_hal.h"
#include "tt_ring.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TT_EVENT_BATCH 64

/* Edges held back waiting for their render window. One block at 48 kHz /
 * 64 frames is 1.3 ms; 256 edges inside that would be a hardware fault. */
#define TT_HELD_MAX 256

struct tt_ctx {
    tt_config            cfg;
    const tt_board_desc *bd;
    const tt_hal_ops    *hal;
    void                *dev;

    int                  num_cv;
    int                  scan_rate;
    uint64_t             scan_period_ns;

    tt_cvring            cv;
    tt_evring            ev;      /* drained by tt_poll_events   */
    tt_evring            tev;     /* drained by tt_trigger_block */
    tt_cal               cal;

    /* tt_trigger_block state. Single consumer, so plain fields. */
    tt_event             held[TT_HELD_MAX];
    int                  nheld;
    float                trig_render[TT_MAX_TRIGGERS];

    pthread_t            thread;
    int                  thread_started;
    _Atomic int          running;

    /* Gate outputs. The audio thread requests, the sampling thread applies. */
    _Atomic uint32_t     gate_level;
    _Atomic uint64_t     gate_at[TT_MAX_GATES];
    _Atomic int          gate_armed[TT_MAX_GATES];
    _Atomic int          gate_armed_level[TT_MAX_GATES];
    uint32_t             gate_applied;

    /* Input levels published by the sampling thread. */
    _Atomic uint32_t     trig_level;
    _Atomic uint32_t     btn_level;
    _Atomic uint64_t     btn_since[TT_MAX_BUTTONS];

    /* Reader-side state. Single consumer, so no atomics needed. */
    float                smooth_hz;
    float                smooth_z[TT_MAX_CV];
    int                  smooth_primed;
    float                last_v[TT_MAX_CV];

    _Atomic uint64_t     n_scans;
    _Atomic uint64_t     n_overruns;
    _Atomic uint64_t     n_events;
    _Atomic uint64_t     scan_ns_sum;
    _Atomic uint64_t     scan_ns_max;
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void sleep_until(uint64_t t_ns)
{
#if defined(__linux__)
    struct timespec ts;
    ts.tv_sec  = (time_t)(t_ns / 1000000000ull);
    ts.tv_nsec = (long)(t_ns % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
#else
    uint64_t now = tt_now_ns();
    if (t_ns > now) {
        uint64_t d = t_ns - now;
        struct timespec ts;
        ts.tv_sec  = (time_t)(d / 1000000000ull);
        ts.tv_nsec = (long)(d % 1000000000ull);
        nanosleep(&ts, NULL);
    }
#endif
}

static float code_to_volts(const tt_cal *cal, int ch, uint16_t code)
{
    return ((float)code - cal->ch[ch].offset) * cal->ch[ch].scale;
}

static void scan_to_volts(const tt_ctx *ctx, const tt_scan *s, float *v, int nc)
{
    int i;
    for (i = 0; i < nc; i++) v[i] = code_to_volts(&ctx->cal, i, s->raw[i]);
}

/* ------------------------------------------------------------------ */
/* sampling thread                                                     */
/* ------------------------------------------------------------------ */

static void apply_gates(tt_ctx *ctx, uint64_t now)
{
    uint32_t want;
    int i;

    /* Promote any scheduled change whose time has arrived. */
    for (i = 0; i < ctx->bd->num_gates; i++) {
        if (atomic_load_explicit(&ctx->gate_armed[i], memory_order_acquire)) {
            uint64_t at = atomic_load_explicit(&ctx->gate_at[i],
                                               memory_order_relaxed);
            if (now >= at) {
                int lvl = atomic_load_explicit(&ctx->gate_armed_level[i],
                                               memory_order_relaxed);
                if (lvl) atomic_fetch_or_explicit(&ctx->gate_level,
                                                  1u << i, memory_order_relaxed);
                else     atomic_fetch_and_explicit(&ctx->gate_level,
                                                   ~(1u << i), memory_order_relaxed);
                atomic_store_explicit(&ctx->gate_armed[i], 0,
                                      memory_order_release);
            }
        }
    }

    want = atomic_load_explicit(&ctx->gate_level, memory_order_relaxed);
    if (want == ctx->gate_applied) return;

    for (i = 0; i < ctx->bd->num_gates; i++) {
        uint32_t bit = 1u << i;
        if ((want & bit) != (ctx->gate_applied & bit))
            ctx->hal->gate_set(ctx->dev, i, (want & bit) ? 1 : 0);
    }
    ctx->gate_applied = want;
}

static void handle_events(tt_ctx *ctx)
{
    tt_event batch[TT_EVENT_BATCH];
    int n, i;

    n = ctx->hal->gpio_poll(ctx->dev, batch, TT_EVENT_BATCH);
    if (n <= 0) return;

    for (i = 0; i < n; i++) {
        tt_event *e = &batch[i];
        uint32_t bit;

        if (e->type == TT_EV_TRIGGER && e->index < TT_MAX_TRIGGERS) {
            bit = 1u << e->index;
            if (e->value) atomic_fetch_or_explicit(&ctx->trig_level, bit,
                                                   memory_order_relaxed);
            else          atomic_fetch_and_explicit(&ctx->trig_level, ~bit,
                                                    memory_order_relaxed);
        } else if (e->type == TT_EV_BUTTON && e->index < TT_MAX_BUTTONS) {
            bit = 1u << e->index;
            if (e->value) {
                atomic_fetch_or_explicit(&ctx->btn_level, bit,
                                         memory_order_relaxed);
                atomic_store_explicit(&ctx->btn_since[e->index], e->time_ns,
                                      memory_order_relaxed);
            } else {
                atomic_fetch_and_explicit(&ctx->btn_level, ~bit,
                                          memory_order_relaxed);
                atomic_store_explicit(&ctx->btn_since[e->index], 0,
                                      memory_order_relaxed);
            }
        }

        /* Two independent consumers: tt_poll_events and tt_trigger_block.
         * Feeding both from one ring would make each steal the other's
         * events, so each gets its own copy. */
        tt_evring_push(&ctx->ev, e);
        if (e->type == TT_EV_TRIGGER) tt_evring_push(&ctx->tev, e);
        atomic_fetch_add_explicit(&ctx->n_events, 1, memory_order_relaxed);
    }
}

static void set_rt_priority(int prio)
{
#if defined(__linux__)
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    /* Failure is expected and survivable without CAP_SYS_NICE or an RT
     * kernel; the thread simply runs at normal priority and the caller sees
     * the resulting overrun count in tt_get_stats. */
    (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#else
    (void)prio;
#endif
}

static void *sample_thread(void *arg)
{
    tt_ctx  *ctx = (tt_ctx *)arg;
    uint16_t adc[TT_MAX_CV];
    uint16_t panel[TT_MAX_CV];
    uint64_t next;
    int i;

    if (ctx->cfg.rt_priority > 0) set_rt_priority(ctx->cfg.rt_priority);

    next = tt_now_ns() + ctx->scan_period_ns;

    while (atomic_load_explicit(&ctx->running, memory_order_acquire)) {
        uint64_t t0 = tt_now_ns();
        uint64_t dt, now;

        memset(adc, 0, sizeof(adc));
        if (ctx->hal->adc_scan(ctx->dev, adc, TT_MAX_CV) == 0) {
            /* Reorder into panel positions so nothing above this layer has
             * to know how the MCP3208 channels are wired to the front. */
            for (i = 0; i < ctx->num_cv; i++)
                panel[i] = adc[ctx->bd->panel_from_adc[i]];
            tt_cvring_push(&ctx->cv, t0, panel, ctx->num_cv);
            atomic_fetch_add_explicit(&ctx->n_scans, 1, memory_order_relaxed);
        }

        handle_events(ctx);

        now = tt_now_ns();
        apply_gates(ctx, now);

        dt = now - t0;
        atomic_fetch_add_explicit(&ctx->scan_ns_sum, dt, memory_order_relaxed);
        {
            uint64_t prev = atomic_load_explicit(&ctx->scan_ns_max,
                                                 memory_order_relaxed);
            while (dt > prev &&
                   !atomic_compare_exchange_weak_explicit(
                        &ctx->scan_ns_max, &prev, dt,
                        memory_order_relaxed, memory_order_relaxed))
                ;
        }

        if (now >= next) {
            /* Missed the deadline. Resynchronise rather than trying to catch
             * up, which would only pile more scans onto a busy core. */
            atomic_fetch_add_explicit(&ctx->n_overruns, 1, memory_order_relaxed);
            next = now + ctx->scan_period_ns;
        } else {
            sleep_until(next);
            next += ctx->scan_period_ns;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

tt_ctx *tt_open(const tt_config *cfg_in, char *err, size_t errlen)
{
    tt_config cfg;
    tt_ctx *ctx;
    const tt_hal_ops *hal;
    char calpath[512];
    int i;

    if (err && errlen) err[0] = '\0';

    if (cfg_in) cfg = *cfg_in;
    else        tt_config_init(&cfg, TT_BOARD_WM8731);

    if (cfg.scan_rate_hz <= 0)  cfg.scan_rate_hz = 4000;
    if (cfg.spi_speed_hz <= 0)  cfg.spi_speed_hz = 4000000;
    if (cfg.ring_scans   <= 0)  cfg.ring_scans   = 1024;
    if (cfg.scan_rate_hz > 100000) cfg.scan_rate_hz = 100000;

    switch (cfg.hal) {
    case TT_HAL_SIM:   hal = tt_hal_sim();   break;
    case TT_HAL_LINUX: hal = tt_hal_linux(); break;
    default:
        hal = tt_hal_linux();
        if (!hal) hal = tt_hal_sim();
        break;
    }
    if (!hal) {
        if (err) snprintf(err, errlen,
                          "requested HAL backend is not available in this build");
        return NULL;
    }

    ctx = (tt_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (err) snprintf(err, errlen, "out of memory");
        return NULL;
    }

    ctx->cfg       = cfg;
    ctx->bd        = tt_board_get(cfg.board);
    ctx->hal       = hal;
    ctx->num_cv    = ctx->bd->num_cv;
    ctx->scan_rate = cfg.scan_rate_hz;
    ctx->scan_period_ns = 1000000000ull / (uint64_t)cfg.scan_rate_hz;

    tt_cal_nominal(&ctx->cal, cfg.board);
    if (cfg.cal_path) {
        (void)tt_cal_load_file(&ctx->cal, cfg.cal_path);
    } else {
        tt_cal_default_path(calpath, sizeof(calpath));
        (void)tt_cal_load_file(&ctx->cal, calpath);
    }

    if (tt_cvring_init(&ctx->cv, (uint32_t)cfg.ring_scans) != 0) {
        if (err) snprintf(err, errlen, "could not allocate CV ring");
        free(ctx);
        return NULL;
    }
    if (tt_evring_init(&ctx->ev, 512) != 0) {
        if (err) snprintf(err, errlen, "could not allocate event ring");
        tt_cvring_free(&ctx->cv);
        free(ctx);
        return NULL;
    }
    if (tt_evring_init(&ctx->tev, 512) != 0) {
        if (err) snprintf(err, errlen, "could not allocate trigger ring");
        tt_evring_free(&ctx->ev);
        tt_cvring_free(&ctx->cv);
        free(ctx);
        return NULL;
    }

    if (hal->open(&ctx->dev, &cfg, ctx->bd, err, errlen) != 0) {
        tt_evring_free(&ctx->tev);
        tt_evring_free(&ctx->ev);
        tt_cvring_free(&ctx->cv);
        free(ctx);
        return NULL;
    }

    for (i = 0; i < TT_MAX_CV; i++) ctx->last_v[i] = 0.0f;

    atomic_store_explicit(&ctx->running, 1, memory_order_release);
    if (pthread_create(&ctx->thread, NULL, sample_thread, ctx) != 0) {
        if (err) snprintf(err, errlen, "could not start sampling thread: %s",
                          strerror(errno));
        atomic_store_explicit(&ctx->running, 0, memory_order_release);
        hal->close(ctx->dev);
        tt_evring_free(&ctx->tev);
        tt_evring_free(&ctx->ev);
        tt_cvring_free(&ctx->cv);
        free(ctx);
        return NULL;
    }
    ctx->thread_started = 1;

    return ctx;
}

void tt_close(tt_ctx *ctx)
{
    int i;

    if (!ctx) return;

    atomic_store_explicit(&ctx->running, 0, memory_order_release);
    if (ctx->thread_started) pthread_join(ctx->thread, NULL);

    /* Leave the module quiet rather than holding whatever the patch left. */
    for (i = 0; i < ctx->bd->num_gates; i++)
        ctx->hal->gate_set(ctx->dev, i, 0);

    ctx->hal->close(ctx->dev);
    tt_evring_free(&ctx->tev);
    tt_evring_free(&ctx->ev);
    tt_cvring_free(&ctx->cv);
    free(ctx);
}

int tt_num_cv(const tt_ctx *ctx)      { return ctx ? ctx->num_cv : 0; }
int tt_scan_rate(const tt_ctx *ctx)   { return ctx ? ctx->scan_rate : 0; }
const char *tt_hal_name(const tt_ctx *ctx)
{
    return ctx ? ctx->hal->name : "none";
}

/* ------------------------------------------------------------------ */
/* CV reading                                                          */
/* ------------------------------------------------------------------ */

static void fill_last(tt_ctx *ctx, float *const *out, int nchan,
                      uint32_t nframes)
{
    int ch;
    uint32_t f;
    for (ch = 0; ch < nchan; ch++) {
        float v;
        if (!out[ch]) continue;
        v = (ch < ctx->num_cv) ? ctx->last_v[ch] : 0.0f;
        for (f = 0; f < nframes; f++) out[ch][f] = v;
    }
}

int tt_cv_block(tt_ctx *ctx, uint64_t t0_ns, double ns_per_frame,
                uint32_t nframes, float *const *out, int nchan)
{
    uint64_t w, lo, idx;
    tt_scan a, b;
    float va[TT_MAX_CV], vb[TT_MAX_CV];
    int have_b, nc, ch, found;
    uint32_t f;
    float coef = 0.0f;

    if (!ctx || !out || nchan <= 0 || nframes == 0) return -1;

    nc = (nchan < ctx->num_cv) ? nchan : ctx->num_cv;

    /* Channels the board does not have read as a hard zero, not as noise. */
    for (ch = ctx->num_cv; ch < nchan; ch++)
        if (out[ch]) memset(out[ch], 0, nframes * sizeof(float));

    w = tt_cvring_count(&ctx->cv);
    if (w == 0) { fill_last(ctx, out, nchan, nframes); return -1; }

    lo = (w > ctx->cv.size) ? (w - ctx->cv.size + 1) : 0;

    found = tt_cvring_find(&ctx->cv, lo, w, t0_ns, &idx);
    if (found < 0) { fill_last(ctx, out, nchan, nframes); return -1; }
    if (tt_cvring_get(&ctx->cv, idx, &a) != 0) {
        fill_last(ctx, out, nchan, nframes);
        return -1;
    }

    have_b = (idx + 1 < w) && (tt_cvring_get(&ctx->cv, idx + 1, &b) == 0);
    if (!have_b) b = a;

    scan_to_volts(ctx, &a, va, nc);
    scan_to_volts(ctx, &b, vb, nc);

    if (ctx->smooth_hz > 0.0f && ns_per_frame > 0.0) {
        double sr = 1e9 / ns_per_frame;
        coef = (float)(1.0 - exp(-2.0 * M_PI * (double)ctx->smooth_hz / sr));
        if (!ctx->smooth_primed) {
            for (ch = 0; ch < nc; ch++) ctx->smooth_z[ch] = va[ch];
            ctx->smooth_primed = 1;
        }
    }

    for (f = 0; f < nframes; f++) {
        double t = (double)t0_ns + (double)f * ns_per_frame;
        double frac = 0.0;

        /* Advance the bracketing pair. Over a normal block this runs a
         * handful of times: at 4 kHz scanning and 64 frames at 48 kHz there
         * are about five scans per block. */
        while (have_b && (double)b.t_ns <= t) {
            tt_scan nb;
            int nhave;

            a = b;
            memcpy(va, vb, sizeof(float) * (size_t)nc);
            idx++;

            nhave = (idx + 1 < w) && (tt_cvring_get(&ctx->cv, idx + 1, &nb) == 0);
            if (nhave) {
                b = nb;
                scan_to_volts(ctx, &b, vb, nc);
                have_b = 1;
            } else {
                b = a;
                memcpy(vb, va, sizeof(float) * (size_t)nc);
                have_b = 0;
            }
        }

        if (b.t_ns > a.t_ns) {
            frac = (t - (double)a.t_ns) / (double)(b.t_ns - a.t_ns);
            if (frac < 0.0)      frac = 0.0;
            else if (frac > 1.0) frac = 1.0;
        }

        for (ch = 0; ch < nc; ch++) {
            float v = va[ch] + (float)frac * (vb[ch] - va[ch]);
            if (coef > 0.0f) {
                ctx->smooth_z[ch] += coef * (v - ctx->smooth_z[ch]);
                v = ctx->smooth_z[ch];
            }
            if (out[ch]) out[ch][f] = v;
            if (f + 1 == nframes) ctx->last_v[ch] = v;
        }
    }

    return 0;
}

int tt_cv_latest(tt_ctx *ctx, float *out, int nchan)
{
    tt_scan s;
    uint64_t w;
    int nc, i;

    if (!ctx || !out || nchan <= 0) return -1;
    nc = (nchan < ctx->num_cv) ? nchan : ctx->num_cv;
    for (i = nc; i < nchan; i++) out[i] = 0.0f;

    w = tt_cvring_count(&ctx->cv);
    if (w == 0 || tt_cvring_get(&ctx->cv, w - 1, &s) != 0) {
        for (i = 0; i < nc; i++) out[i] = ctx->last_v[i];
        return -1;
    }
    for (i = 0; i < nc; i++) out[i] = code_to_volts(&ctx->cal, i, s.raw[i]);
    return 0;
}

int tt_cv_raw(tt_ctx *ctx, uint16_t *out, int nchan)
{
    tt_scan s;
    uint64_t w;
    int nc, i;

    if (!ctx || !out || nchan <= 0) return -1;
    nc = (nchan < ctx->num_cv) ? nchan : ctx->num_cv;
    for (i = nc; i < nchan; i++) out[i] = 0;

    w = tt_cvring_count(&ctx->cv);
    if (w == 0 || tt_cvring_get(&ctx->cv, w - 1, &s) != 0) {
        for (i = 0; i < nc; i++) out[i] = 0;
        return -1;
    }
    for (i = 0; i < nc; i++) out[i] = s.raw[i];
    return 0;
}

void tt_set_cv_smoothing(tt_ctx *ctx, float cutoff_hz)
{
    if (!ctx) return;
    if (cutoff_hz < 0.0f) cutoff_hz = 0.0f;
    ctx->smooth_hz = cutoff_hz;
    ctx->smooth_primed = 0;
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

int tt_poll_events(tt_ctx *ctx, tt_event *buf, int max)
{
    if (!ctx || !buf || max <= 0) return 0;
    return tt_evring_drain(&ctx->ev, buf, max);
}

int tt_trigger_block(tt_ctx *ctx, uint64_t t0_ns, double ns_per_frame,
                     uint32_t nframes, float *const *out, int nchan)
{
    double  wend_d;
    uint64_t wend;
    int nc, ch, i, keep;
    uint32_t f;

    if (!ctx || !out || nchan <= 0 || nframes == 0 || ns_per_frame <= 0.0)
        return -1;

    nc = ctx->bd->num_triggers;
    if (nchan < nc) nc = nchan;
    for (ch = nc; ch < nchan; ch++)
        if (out[ch]) memset(out[ch], 0, nframes * sizeof(float));

    wend_d = (double)t0_ns + ns_per_frame * (double)nframes;
    wend   = (uint64_t)wend_d;

    /* Take everything the sampling thread has produced. Edges beyond this
     * window stay held for the next call. */
    if (ctx->nheld < TT_HELD_MAX)
        ctx->nheld += tt_evring_drain(&ctx->tev, ctx->held + ctx->nheld,
                                      TT_HELD_MAX - ctx->nheld);

    /* Start each channel at the level it ended the previous block on. */
    for (ch = 0; ch < nc; ch++) {
        if (!out[ch]) continue;
        for (f = 0; f < nframes; f++) out[ch][f] = ctx->trig_render[ch];
    }

    keep = 0;
    for (i = 0; i < ctx->nheld; i++) {
        tt_event *e = &ctx->held[i];
        uint32_t off;
        float lvl;

        if (e->time_ns >= wend) {                 /* not yet; hold it */
            ctx->held[keep++] = *e;
            continue;
        }
        if (e->index >= (uint8_t)nc) continue;    /* channel not requested */

        if (e->time_ns <= t0_ns) {
            /* Late arrival. Placing it at frame 0 loses sub-block accuracy
             * for this one edge, which is still better than dropping it. */
            off = 0;
        } else {
            double d = ((double)(e->time_ns - t0_ns)) / ns_per_frame;
            off = (d >= (double)nframes) ? nframes - 1 : (uint32_t)d;
        }

        lvl = e->value ? 1.0f : 0.0f;
        ctx->trig_render[e->index] = lvl;

        if (out[e->index])
            for (f = off; f < nframes; f++) out[e->index][f] = lvl;
    }
    ctx->nheld = keep;

    return 0;
}

int tt_gate_block(tt_ctx *ctx, uint64_t t0_ns, double ns_per_frame,
                  uint32_t nframes, const float *const *in, int nchan)
{
    int nc, ch;
    uint32_t f;

    if (!ctx || !in || nchan <= 0 || nframes == 0) return -1;

    nc = ctx->bd->num_gates;
    if (nchan < nc) nc = nchan;

    for (ch = 0; ch < nc; ch++) {
        int cur;
        if (!in[ch]) continue;
        cur = (int)((atomic_load_explicit(&ctx->gate_level,
                                          memory_order_relaxed) >> ch) & 1u);
        for (f = 0; f < nframes; f++) {
            int want = in[ch][f] > 0.5f ? 1 : 0;
            if (want != cur) {
                uint64_t at = t0_ns + (uint64_t)(ns_per_frame * (double)f);
                tt_gate_set_at(ctx, ch, want, at);
                cur = want;
            }
        }
    }
    return 0;
}

int tt_trigger_state(tt_ctx *ctx, int index)
{
    uint32_t lv;
    if (!ctx || index < 0 || index >= ctx->bd->num_triggers) return 0;
    lv = atomic_load_explicit(&ctx->trig_level, memory_order_relaxed);
    return (lv >> index) & 1u;
}

int tt_button_state(tt_ctx *ctx, int index)
{
    uint32_t lv;
    if (!ctx || index < 0 || index >= ctx->bd->num_buttons) return 0;
    lv = atomic_load_explicit(&ctx->btn_level, memory_order_relaxed);
    return (lv >> index) & 1u;
}

double tt_button_held_ms(tt_ctx *ctx, int index)
{
    uint64_t since, now;

    if (!tt_button_state(ctx, index)) return 0.0;
    since = atomic_load_explicit(&ctx->btn_since[index], memory_order_relaxed);
    if (since == 0) return 0.0;
    now = tt_now_ns();
    if (now <= since) return 0.0;
    return (double)(now - since) / 1e6;
}

/* ------------------------------------------------------------------ */
/* gates                                                               */
/* ------------------------------------------------------------------ */

int tt_gate_set(tt_ctx *ctx, int index, int on)
{
    if (!ctx || index < 0 || index >= ctx->bd->num_gates) return -1;
    atomic_store_explicit(&ctx->gate_armed[index], 0, memory_order_release);
    if (on) atomic_fetch_or_explicit(&ctx->gate_level, 1u << index,
                                     memory_order_relaxed);
    else    atomic_fetch_and_explicit(&ctx->gate_level, ~(1u << index),
                                      memory_order_relaxed);
    return 0;
}

int tt_gate_set_at(tt_ctx *ctx, int index, int on, uint64_t time_ns)
{
    if (!ctx || index < 0 || index >= ctx->bd->num_gates) return -1;
    if (time_ns <= tt_now_ns()) return tt_gate_set(ctx, index, on);

    atomic_store_explicit(&ctx->gate_armed[index], 0, memory_order_release);
    atomic_store_explicit(&ctx->gate_armed_level[index], on ? 1 : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&ctx->gate_at[index], time_ns, memory_order_relaxed);
    atomic_store_explicit(&ctx->gate_armed[index], 1, memory_order_release);
    return 0;
}

/* ------------------------------------------------------------------ */
/* calibration                                                         */
/* ------------------------------------------------------------------ */

void tt_cal_get(const tt_ctx *ctx, tt_cal *out)
{
    if (ctx && out) *out = ctx->cal;
}

void tt_cal_set(tt_ctx *ctx, const tt_cal *cal)
{
    if (ctx && cal) ctx->cal = *cal;
}

/* ------------------------------------------------------------------ */
/* stats                                                               */
/* ------------------------------------------------------------------ */

void tt_get_stats(const tt_ctx *ctx, tt_stats *out)
{
    uint64_t n, sum;

    if (!ctx || !out) return;
    memset(out, 0, sizeof(*out));

    n   = atomic_load_explicit(&ctx->n_scans, memory_order_relaxed);
    sum = atomic_load_explicit(&ctx->scan_ns_sum, memory_order_relaxed);

    out->scans          = n;
    out->scan_overruns  = atomic_load_explicit(&ctx->n_overruns,
                                               memory_order_relaxed);
    out->events         = atomic_load_explicit(&ctx->n_events,
                                               memory_order_relaxed);
    out->events_dropped = tt_evring_dropped(&ctx->ev);
    out->scan_us_mean   = n ? ((double)sum / (double)n) / 1000.0 : 0.0;
    out->scan_us_max    = (double)atomic_load_explicit(&ctx->scan_ns_max,
                                                       memory_order_relaxed)
                          / 1000.0;
}

void tt_reset_stats(tt_ctx *ctx)
{
    if (!ctx) return;
    atomic_store_explicit(&ctx->n_scans, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->n_overruns, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->n_events, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->scan_ns_sum, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->scan_ns_max, 0, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* simulation passthrough                                              */
/* ------------------------------------------------------------------ */

void tt_sim_set_cv(tt_ctx *ctx, int channel, float volts)
{
    double code;
    int adc_ch;

    if (!ctx || !ctx->hal->sim_set_raw) return;
    if (channel < 0 || channel >= ctx->num_cv) return;
    if (ctx->cal.ch[channel].scale == 0.0f) return;

    /* Invert the calibration so a test can set volts and read the same volts
     * back through the whole interpolation path. */
    code = (double)ctx->cal.ch[channel].offset +
           (double)volts / (double)ctx->cal.ch[channel].scale;
    if (code < 0.0)    code = 0.0;
    if (code > 4095.0) code = 4095.0;

    adc_ch = ctx->bd->panel_from_adc[channel];
    ctx->hal->sim_set_raw(ctx->dev, adc_ch, (uint16_t)(code + 0.5));
}

int tt_sim_event(tt_ctx *ctx, tt_event_type type, int index, int value)
{
    tt_event ev;

    if (!ctx || !ctx->hal->sim_event) return -1;
    memset(&ev, 0, sizeof(ev));
    ev.time_ns = tt_now_ns();
    ev.type    = (uint8_t)type;
    ev.index   = (uint8_t)index;
    ev.value   = value ? 1 : 0;
    return ctx->hal->sim_event(ctx->dev, &ev);
}

int tt_sim_gate_state(tt_ctx *ctx, int index)
{
    if (!ctx || !ctx->hal->sim_gate_state) return -1;
    return ctx->hal->sim_gate_state(ctx->dev, index);
}
