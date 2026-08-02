/*
 * [tedium~] - Pure Data binding for libtedium.
 *
 * Replaces terminal_tedium_adc, tedium_input, tedium_output and
 * tedium_switch with one object, and changes what the module can do:
 *
 *   - CV arrives as signals, so it can be patched straight into [vcf~],
 *     [phasor~] or [*~] with no [line~] smoothing and no zipper noise.
 *   - Triggers arrive as signals with edges on the exact sample the kernel
 *     timestamped them, instead of banged from a 1 ms poll that quantised to
 *     the DSP block and dropped short pulses.
 *   - No SPI syscall ever runs on Pd's scheduler thread.
 *
 * Inlets   2 signal, driving gate outputs 1 and 2
 * Outlets  N signal  CV in volts (6 or 8, by board)
 *          4 signal  trigger inputs, 1 while high
 *          1 control button events as: button <index> <1|0> <held_ms>
 *
 * Messages
 *   smooth <hz>     one-pole on CV; 0 (default) disables
 *   cal <path>      reload calibration
 *   verbose <0|1>   report xruns and dropped events to the console
 *   print           dump status to the console
 *
 * Creation args: [tedium~ wm8731] or [tedium~ pcm5102a], plus optional
 * "rate <hz>" and "prio <1..99>".
 *
 * One hardware context is shared by every instance, so putting [tedium~] in
 * more than one subpatch is safe. The first instance opens the device; the
 * last one to go closes it.
 */

#include "m_pd.h"
#include "tedium/tedium.h"

#include <string.h>

static t_class *tedium_tilde_class;

/* ------------------------------------------------------------------ */
/* shared context                                                      */
/* ------------------------------------------------------------------ */

static tt_ctx *g_ctx;
static int     g_refs;
static char    g_err[TT_ERRLEN];

static tt_ctx *shared_acquire(const tt_config *cfg)
{
    if (!g_ctx) {
        g_ctx = tt_open(cfg, g_err, sizeof(g_err));
        if (!g_ctx) return NULL;
    }
    g_refs++;
    return g_ctx;
}

static void shared_release(void)
{
    if (--g_refs <= 0) {
        tt_close(g_ctx);
        g_ctx = NULL;
        g_refs = 0;
    }
}

/* ------------------------------------------------------------------ */

typedef struct _tedium_tilde {
    t_object    x_obj;
    t_float     x_f;             /* dummy for CLASS_MAINSIGNALIN */

    tt_ctx     *x_ctx;
    tt_board    x_board;
    int         x_ncv;

    t_outlet   *x_cv[TT_MAX_CV];
    t_outlet   *x_trig[TT_MAX_TRIGGERS];
    t_outlet   *x_ctl;

    t_inlet    *x_gate_in;       /* second gate inlet; first is the main one */

    /* Signal vectors captured at dsp time. Per instance, not static: two
     * [tedium~] objects in one patch would otherwise share one array and
     * scribble over each other's pointers. */
    t_sample   *x_io[TT_MAX_GATES + TT_MAX_CV + TT_MAX_TRIGGERS];
    int         x_nio;

    tt_timebase x_tb;
    int         x_tb_ready;
    double      x_sr;

    t_clock    *x_clock;         /* control-rate event pump */
    int         x_verbose;
    uint64_t    x_last_overruns;
    uint64_t    x_last_dropped;
} t_tedium_tilde;

/* ------------------------------------------------------------------ */
/* perform                                                             */
/* ------------------------------------------------------------------ */

static t_int *tedium_tilde_perform(t_int *w)
{
    t_tedium_tilde *x = (t_tedium_tilde *)(w[1]);
    int n = (int)(w[2]);
    t_sample **io = x->x_io;

    float *cv[TT_MAX_CV];
    float *trig[TT_MAX_TRIGGERS];
    const float *gate[TT_MAX_GATES];
    uint64_t block_start, render_t0;
    double ns_per_frame;
    int i;

    /* io layout: [0..1] gate inlets, then CV outs, then trigger outs. */
    for (i = 0; i < TT_MAX_GATES; i++) gate[i] = (const float *)io[i];
    for (i = 0; i < x->x_ncv; i++) cv[i] = (float *)io[TT_MAX_GATES + i];
    for (i = 0; i < TT_MAX_TRIGGERS; i++)
        trig[i] = (float *)io[TT_MAX_GATES + x->x_ncv + i];

    if (!x->x_ctx) {
        for (i = 0; i < x->x_ncv; i++) memset(cv[i], 0, sizeof(float) * n);
        for (i = 0; i < TT_MAX_TRIGGERS; i++)
            memset(trig[i], 0, sizeof(float) * n);
        return (w + 3);
    }

    ns_per_frame = 1e9 / x->x_sr;

    /* Pd exposes no hardware timestamp, so recover a stable block time from
     * the callback arrivals. Reading the clock raw would put the callback's
     * own jitter straight onto every trigger edge. */
    block_start = tt_timebase_tick(&x->x_tb, tt_now_ns(), (uint32_t)n);

    /* Render one block in arrears so edges that arrived during the previous
     * block can be placed on their exact sample. Constant latency; the
     * alternative is jitter, which is worse. */
    render_t0 = block_start - (uint64_t)(ns_per_frame * (double)n);

    tt_cv_block(x->x_ctx, render_t0, ns_per_frame, (uint32_t)n, cv, x->x_ncv);
    tt_trigger_block(x->x_ctx, render_t0, ns_per_frame, (uint32_t)n,
                     trig, TT_MAX_TRIGGERS);
    tt_gate_block(x->x_ctx, block_start, ns_per_frame, (uint32_t)n,
                  gate, TT_MAX_GATES);

    return (w + 3);
}

static void tedium_tilde_dsp(t_tedium_tilde *x, t_signal **sp)
{
    int i;

    x->x_nio = TT_MAX_GATES + x->x_ncv + TT_MAX_TRIGGERS;
    x->x_sr  = sp[0]->s_sr > 0 ? sp[0]->s_sr : 48000.0;

    /* The DLL is seeded per block size; a [block~] change re-runs dsp. */
    tt_timebase_init(&x->x_tb, x->x_sr, (uint32_t)sp[0]->s_n, 0.5);

    for (i = 0; i < x->x_nio; i++) x->x_io[i] = sp[i]->s_vec;

    dsp_add(tedium_tilde_perform, 2, x, (t_int)sp[0]->s_n);
}

/* ------------------------------------------------------------------ */
/* control rate                                                        */
/* ------------------------------------------------------------------ */

/*
 * Buttons are a control-rate concern, so they are pumped from a clock rather
 * than from the perform routine. 5 ms is far finer than a finger.
 */
static void tedium_tilde_tick(t_tedium_tilde *x)
{
    tt_event ev[32];
    int n, i;

    if (x->x_ctx) {
        n = tt_poll_events(x->x_ctx, ev, 32);
        for (i = 0; i < n; i++) {
            t_atom a[3];
            if (ev[i].type != TT_EV_BUTTON) continue;
            SETFLOAT(&a[0], (t_float)(ev[i].index + 1));
            SETFLOAT(&a[1], (t_float)ev[i].value);
            SETFLOAT(&a[2], (t_float)(ev[i].value
                     ? 0.0 : tt_button_held_ms(x->x_ctx, ev[i].index)));
            outlet_anything(x->x_ctl, gensym("button"), 3, a);
        }

        if (x->x_verbose) {
            tt_stats st;
            tt_get_stats(x->x_ctx, &st);
            if (st.scan_overruns > x->x_last_overruns) {
                post("tedium~: %llu scan overruns",
                     (unsigned long long)(st.scan_overruns - x->x_last_overruns));
                x->x_last_overruns = st.scan_overruns;
            }
            if (st.events_dropped > x->x_last_dropped) {
                pd_error(x, "tedium~: dropped %llu GPIO events",
                     (unsigned long long)(st.events_dropped - x->x_last_dropped));
                x->x_last_dropped = st.events_dropped;
            }
        }
    }
    clock_delay(x->x_clock, 5.0);
}

/* ------------------------------------------------------------------ */
/* messages                                                            */
/* ------------------------------------------------------------------ */

static void tedium_tilde_smooth(t_tedium_tilde *x, t_floatarg hz)
{
    if (x->x_ctx) tt_set_cv_smoothing(x->x_ctx, (float)hz);
}

static void tedium_tilde_verbose(t_tedium_tilde *x, t_floatarg on)
{
    x->x_verbose = (on != 0);
}

static void tedium_tilde_cal(t_tedium_tilde *x, t_symbol *s)
{
    tt_cal cal;
    if (!x->x_ctx) return;
    tt_cal_nominal(&cal, x->x_board);
    if (tt_cal_load_file(&cal, s->s_name) != 0) {
        pd_error(x, "tedium~: could not read calibration '%s'", s->s_name);
        return;
    }
    tt_cal_set(x->x_ctx, &cal);
    post("tedium~: loaded calibration from %s", s->s_name);
}

static void tedium_tilde_print(t_tedium_tilde *x)
{
    tt_stats st;

    if (!x->x_ctx) {
        pd_error(x, "tedium~: not open (%s)", g_err[0] ? g_err : "unknown");
        return;
    }
    tt_get_stats(x->x_ctx, &st);
    post("tedium~: board %s, %d CV, backend %s, %d Hz scan",
         tt_board_name(x->x_board), x->x_ncv, tt_hal_name(x->x_ctx),
         tt_scan_rate(x->x_ctx));
    post("  scans %llu, overruns %llu, events %llu (dropped %llu)",
         (unsigned long long)st.scans,
         (unsigned long long)st.scan_overruns,
         (unsigned long long)st.events,
         (unsigned long long)st.events_dropped);
    post("  scan time mean %.1f us, max %.1f us",
         st.scan_us_mean, st.scan_us_max);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void *tedium_tilde_new(t_symbol *s, int argc, t_atom *argv)
{
    t_tedium_tilde *x = (t_tedium_tilde *)pd_new(tedium_tilde_class);
    tt_config cfg;
    int i;

    tt_config_init(&cfg, TT_BOARD_WM8731);

    for (i = 0; i < argc; i++) {
        if (argv[i].a_type == A_SYMBOL) {
            const char *a = atom_getsymbol(&argv[i])->s_name;
            if (!strcmp(a, "pcm5102a")) cfg.board = TT_BOARD_PCM5102A;
            else if (!strcmp(a, "wm8731")) cfg.board = TT_BOARD_WM8731;
            else if (!strcmp(a, "sim")) cfg.hal = TT_HAL_SIM;
            else if (!strcmp(a, "rate") && i + 1 < argc)
                cfg.scan_rate_hz = (int)atom_getfloat(&argv[++i]);
            else if (!strcmp(a, "prio") && i + 1 < argc)
                cfg.rt_priority = (int)atom_getfloat(&argv[++i]);
            else
                pd_error(x, "tedium~: unknown argument '%s'", a);
        }
    }

    x->x_board = cfg.board;
    x->x_ncv   = tt_board_num_cv(cfg.board);
    x->x_sr    = 48000.0;
    x->x_verbose = 0;

    /* Two signal inlets for the gate outputs. The leftmost is created by
     * CLASS_MAINSIGNALIN. */
    x->x_gate_in = inlet_new(&x->x_obj, &x->x_obj.ob_pd,
                             &s_signal, &s_signal);

    for (i = 0; i < x->x_ncv; i++)
        x->x_cv[i] = outlet_new(&x->x_obj, &s_signal);
    for (i = 0; i < TT_MAX_TRIGGERS; i++)
        x->x_trig[i] = outlet_new(&x->x_obj, &s_signal);
    x->x_ctl = outlet_new(&x->x_obj, &s_anything);

    x->x_ctx = shared_acquire(&cfg);
    if (!x->x_ctx) {
        pd_error(x, "tedium~: %s", g_err);
        pd_error(x, "tedium~: running silent; the object still works as a "
                    "no-op so patches load");
    } else {
        post("tedium~: %s board, %s backend, %d Hz scan",
             tt_board_name(x->x_board), tt_hal_name(x->x_ctx),
             tt_scan_rate(x->x_ctx));
    }

    x->x_clock = clock_new(x, (t_method)tedium_tilde_tick);
    clock_delay(x->x_clock, 5.0);

    (void)s;
    return (void *)x;
}

static void tedium_tilde_free(t_tedium_tilde *x)
{
    if (x->x_clock) clock_free(x->x_clock);
    if (x->x_gate_in) inlet_free(x->x_gate_in);
    if (x->x_ctx) shared_release();
}

void tedium_tilde_setup(void)
{
    tedium_tilde_class = class_new(gensym("tedium~"),
        (t_newmethod)tedium_tilde_new,
        (t_method)tedium_tilde_free,
        sizeof(t_tedium_tilde),
        CLASS_DEFAULT,
        A_GIMME, 0);

    CLASS_MAINSIGNALIN(tedium_tilde_class, t_tedium_tilde, x_f);

    class_addmethod(tedium_tilde_class, (t_method)tedium_tilde_dsp,
                    gensym("dsp"), A_CANT, 0);
    class_addmethod(tedium_tilde_class, (t_method)tedium_tilde_smooth,
                    gensym("smooth"), A_DEFFLOAT, 0);
    class_addmethod(tedium_tilde_class, (t_method)tedium_tilde_verbose,
                    gensym("verbose"), A_DEFFLOAT, 0);
    class_addmethod(tedium_tilde_class, (t_method)tedium_tilde_cal,
                    gensym("cal"), A_DEFSYM, 0);
    class_addmethod(tedium_tilde_class, (t_method)tedium_tilde_print,
                    gensym("print"), 0);

    post("tedium~ %s - terminal tedium I/O", tt_version());
}
