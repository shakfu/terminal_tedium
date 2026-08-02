/*
 * Tedium.chug - ChucK binding for libtedium.
 *
 * Building needs a ChucK source tree for the headers, which have no system
 * install location:
 *
 *     make -C bindings chuck CK_INCLUDE=/path/to/chuck/core
 *
 * Built and run against ChucK 1.5.5.7. Note that the selector method is
 * called .select() and not .chan(): UGen already defines .chan() with a
 * different return type, and ChucK refuses to load a chugin that collides
 * with it.
 *
 * ChucK also speaks JACK, so bindings/jack/tedium-jack is an alternative
 * that needs no chugin at all: run it and read CV as adc channels.
 *
 * Classes
 *   TediumCV    UGen. Outputs one CV input as an audio signal, in volts.
 *                 .select(int)  choose channel, 0-based
 *   TediumTrig  UGen. Outputs one trigger input as a gate, 1.0 while high.
 *                 .select(int)
 *   TediumGate  UGen. Its input drives a gate output, threshold 0.5.
 *                 .select(int)
 *   Tedium      Control-rate access, no signal graph involvement.
 *                 .cv(int) .trig(int) .button(int) .hold(int)
 *                 .gate(int, int) .smooth(float) .numCV()
 */

#include "chuck_dl.h"
#include "chuck_def.h"
/* Chuck_Object is only forward-declared in chuck_dl.h as of 1.5.x, but
 * OBJ_MEMBER_UINT dereferences it. */
#include "chuck_oo.h"

#include "tedium/tedium.h"

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* shared context                                                      */
/* ------------------------------------------------------------------ */

/*
 * Opened on first use by any class, closed when the last object goes.
 * ChucK gives no module-unload hook that fires reliably, so the refcount
 * is what stops the sampling thread.
 */
static tt_ctx *g_ctx;
static int     g_refs;
static char    g_err[TT_ERRLEN];

static tt_ctx *ted_acquire()
{
    if (!g_ctx) {
        tt_config cfg;
        const char *env;

        tt_config_init(&cfg, TT_BOARD_WM8731);

        env = getenv("TEDIUM_BOARD");
        if (env && !strcmp(env, "pcm5102a")) cfg.board = TT_BOARD_PCM5102A;
        env = getenv("TEDIUM_HAL");
        if (env && !strcmp(env, "sim")) cfg.hal = TT_HAL_SIM;
        env = getenv("TEDIUM_SCAN_HZ");
        if (env && *env) cfg.scan_rate_hz = atoi(env);
        env = getenv("TEDIUM_RT_PRIO");
        if (env && *env) cfg.rt_priority = atoi(env);
        env = getenv("TEDIUM_CAL");
        if (env && *env) cfg.cal_path = env;

        g_ctx = tt_open(&cfg, g_err, sizeof(g_err));
        if (!g_ctx) return NULL;
    }
    g_refs++;
    return g_ctx;
}

static void ted_release()
{
    if (--g_refs <= 0) {
        tt_close(g_ctx);
        g_ctx = NULL;
        g_refs = 0;
    }
}

/* ------------------------------------------------------------------ */
/* per-object state                                                    */
/* ------------------------------------------------------------------ */

/*
 * ChucK ticks UGens one sample at a time, but libtedium renders blocks --
 * a per-sample tt_cv_block would run a binary search for every frame. So
 * each object buffers a block and serves from it.
 */
#define TED_BLOCK 64

struct TedUGen {
    tt_ctx     *ctx;
    int         chan;
    float       buf[TED_BLOCK];
    int         pos;
    tt_timebase tb;
    int         tb_inited;
    double      sr;

    TedUGen(double samplerate)
        : ctx(NULL), chan(0), pos(TED_BLOCK), tb_inited(0), sr(samplerate)
    {
        memset(buf, 0, sizeof(buf));
        ctx = ted_acquire();
    }
    ~TedUGen() { if (ctx) ted_release(); }

    uint64_t next_block_time()
    {
        if (!tb_inited) {
            tt_timebase_init(&tb, sr, TED_BLOCK, 0.5);
            tb_inited = 1;
        }
        return tt_timebase_tick(&tb, tt_now_ns(), TED_BLOCK);
    }
};

/* ------------------------------------------------------------------ */
/* TediumCV                                                            */
/* ------------------------------------------------------------------ */

static t_CKUINT tedcv_offset = 0;

CK_DLL_CTOR(tedcv_ctor)
{
    OBJ_MEMBER_UINT(SELF, tedcv_offset) = 0;
    TedUGen *o = new TedUGen(API->vm->srate(VM));
    OBJ_MEMBER_UINT(SELF, tedcv_offset) = (t_CKUINT)o;
}

CK_DLL_DTOR(tedcv_dtor)
{
    TedUGen *o = (TedUGen *)OBJ_MEMBER_UINT(SELF, tedcv_offset);
    if (o) { delete o; OBJ_MEMBER_UINT(SELF, tedcv_offset) = 0; }
}

CK_DLL_TICK(tedcv_tick)
{
    TedUGen *o = (TedUGen *)OBJ_MEMBER_UINT(SELF, tedcv_offset);

    if (!o || !o->ctx) { *out = 0; return TRUE; }

    if (o->pos >= TED_BLOCK) {
        float *ptr[TT_MAX_CV];
        float scratch[TT_MAX_CV][TED_BLOCK];
        double ns_per_frame = 1e9 / o->sr;
        uint64_t t0 = o->next_block_time();
        uint64_t render = t0 - (uint64_t)(ns_per_frame * (double)TED_BLOCK);
        int nc = tt_num_cv(o->ctx);
        int i;

        for (i = 0; i < nc; i++) ptr[i] = scratch[i];
        tt_cv_block(o->ctx, render, ns_per_frame, TED_BLOCK, ptr, nc);

        if (o->chan >= 0 && o->chan < nc)
            memcpy(o->buf, scratch[o->chan], sizeof(o->buf));
        else
            memset(o->buf, 0, sizeof(o->buf));
        o->pos = 0;
    }

    *out = (SAMPLE)o->buf[o->pos++];
    return TRUE;
}

CK_DLL_MFUN(tedcv_chan)
{
    TedUGen *o = (TedUGen *)OBJ_MEMBER_UINT(SELF, tedcv_offset);
    t_CKINT c = GET_NEXT_INT(ARGS);
    if (o) { o->chan = (int)c; o->pos = TED_BLOCK; }
    RETURN->v_int = c;
}

/* ------------------------------------------------------------------ */
/* TediumTrig                                                          */
/* ------------------------------------------------------------------ */

static t_CKUINT tedtrig_offset = 0;

CK_DLL_CTOR(tedtrig_ctor)
{
    TedUGen *o = new TedUGen(API->vm->srate(VM));
    OBJ_MEMBER_UINT(SELF, tedtrig_offset) = (t_CKUINT)o;
}

CK_DLL_DTOR(tedtrig_dtor)
{
    TedUGen *o = (TedUGen *)OBJ_MEMBER_UINT(SELF, tedtrig_offset);
    if (o) { delete o; OBJ_MEMBER_UINT(SELF, tedtrig_offset) = 0; }
}

CK_DLL_TICK(tedtrig_tick)
{
    TedUGen *o = (TedUGen *)OBJ_MEMBER_UINT(SELF, tedtrig_offset);

    if (!o || !o->ctx) { *out = 0; return TRUE; }

    if (o->pos >= TED_BLOCK) {
        float *ptr[TT_MAX_TRIGGERS];
        float scratch[TT_MAX_TRIGGERS][TED_BLOCK];
        double ns_per_frame = 1e9 / o->sr;
        uint64_t t0 = o->next_block_time();
        uint64_t render = t0 - (uint64_t)(ns_per_frame * (double)TED_BLOCK);
        int i;

        for (i = 0; i < TT_MAX_TRIGGERS; i++) ptr[i] = scratch[i];
        tt_trigger_block(o->ctx, render, ns_per_frame, TED_BLOCK,
                         ptr, TT_MAX_TRIGGERS);

        if (o->chan >= 0 && o->chan < TT_MAX_TRIGGERS)
            memcpy(o->buf, scratch[o->chan], sizeof(o->buf));
        else
            memset(o->buf, 0, sizeof(o->buf));
        o->pos = 0;
    }

    *out = (SAMPLE)o->buf[o->pos++];
    return TRUE;
}

CK_DLL_MFUN(tedtrig_chan)
{
    TedUGen *o = (TedUGen *)OBJ_MEMBER_UINT(SELF, tedtrig_offset);
    t_CKINT c = GET_NEXT_INT(ARGS);
    if (o) { o->chan = (int)c; o->pos = TED_BLOCK; }
    RETURN->v_int = c;
}

/* ------------------------------------------------------------------ */
/* TediumGate                                                          */
/* ------------------------------------------------------------------ */

static t_CKUINT tedgate_offset = 0;

struct TedGate {
    tt_ctx *ctx;
    int     chan;
    int     level;
    TedGate() : ctx(NULL), chan(0), level(0) { ctx = ted_acquire(); }
    ~TedGate() { if (ctx) ted_release(); }
};

CK_DLL_CTOR(tedgate_ctor)
{
    OBJ_MEMBER_UINT(SELF, tedgate_offset) = (t_CKUINT)(new TedGate());
}

CK_DLL_DTOR(tedgate_dtor)
{
    TedGate *o = (TedGate *)OBJ_MEMBER_UINT(SELF, tedgate_offset);
    if (o) { delete o; OBJ_MEMBER_UINT(SELF, tedgate_offset) = 0; }
}

CK_DLL_TICK(tedgate_tick)
{
    TedGate *o = (TedGate *)OBJ_MEMBER_UINT(SELF, tedgate_offset);
    int want;

    if (!o || !o->ctx) { *out = in; return TRUE; }

    /* Output granularity is the scan period regardless, so a per-sample
     * comparison plus a change check is all that is useful here. */
    want = (in > 0.5f) ? 1 : 0;
    if (want != o->level) {
        tt_gate_set(o->ctx, o->chan, want);
        o->level = want;
    }
    *out = in;
    return TRUE;
}

CK_DLL_MFUN(tedgate_chan)
{
    TedGate *o = (TedGate *)OBJ_MEMBER_UINT(SELF, tedgate_offset);
    t_CKINT c = GET_NEXT_INT(ARGS);
    if (o) o->chan = (int)c;
    RETURN->v_int = c;
}

/* ------------------------------------------------------------------ */
/* Tedium : control rate                                               */
/* ------------------------------------------------------------------ */

static t_CKUINT tedctl_offset = 0;

CK_DLL_CTOR(tedctl_ctor)
{
    OBJ_MEMBER_UINT(SELF, tedctl_offset) = (t_CKUINT)ted_acquire();
}

CK_DLL_DTOR(tedctl_dtor)
{
    if (OBJ_MEMBER_UINT(SELF, tedctl_offset)) {
        ted_release();
        OBJ_MEMBER_UINT(SELF, tedctl_offset) = 0;
    }
}

CK_DLL_MFUN(tedctl_cv)
{
    t_CKINT c = GET_NEXT_INT(ARGS);
    float v[TT_MAX_CV];

    RETURN->v_float = 0.0;
    if (!g_ctx) return;
    if (tt_cv_latest(g_ctx, v, TT_MAX_CV) != 0) return;
    if (c >= 0 && c < tt_num_cv(g_ctx)) RETURN->v_float = (t_CKFLOAT)v[c];
}

CK_DLL_MFUN(tedctl_trig)
{
    t_CKINT c = GET_NEXT_INT(ARGS);
    RETURN->v_int = g_ctx ? tt_trigger_state(g_ctx, (int)c) : 0;
}

CK_DLL_MFUN(tedctl_button)
{
    t_CKINT c = GET_NEXT_INT(ARGS);
    RETURN->v_int = g_ctx ? tt_button_state(g_ctx, (int)c) : 0;
}

CK_DLL_MFUN(tedctl_hold)
{
    t_CKINT c = GET_NEXT_INT(ARGS);
    RETURN->v_float = g_ctx ? (t_CKFLOAT)tt_button_held_ms(g_ctx, (int)c) : 0.0;
}

CK_DLL_MFUN(tedctl_gate)
{
    t_CKINT c = GET_NEXT_INT(ARGS);
    t_CKINT v = GET_NEXT_INT(ARGS);
    if (g_ctx) tt_gate_set(g_ctx, (int)c, (int)v);
    RETURN->v_int = v;
}

CK_DLL_MFUN(tedctl_smooth)
{
    t_CKFLOAT hz = GET_NEXT_FLOAT(ARGS);
    if (g_ctx) tt_set_cv_smoothing(g_ctx, (float)hz);
    RETURN->v_float = hz;
}

CK_DLL_MFUN(tedctl_numcv)
{
    RETURN->v_int = g_ctx ? tt_num_cv(g_ctx) : 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

CK_DLL_QUERY(Tedium)
{
    QUERY->setname(QUERY, "Tedium");

    /* ---- TediumCV ---- */
    QUERY->begin_class(QUERY, "TediumCV", "UGen");
    QUERY->add_ctor(QUERY, tedcv_ctor);
    QUERY->add_dtor(QUERY, tedcv_dtor);
    QUERY->add_ugen_func(QUERY, tedcv_tick, NULL, 0, 1);
    QUERY->add_mfun(QUERY, tedcv_chan, "int", "select");
    QUERY->add_arg(QUERY, "int", "c");
    tedcv_offset = QUERY->add_mvar(QUERY, "int", "@ted_cv", FALSE);
    QUERY->end_class(QUERY);

    /* ---- TediumTrig ---- */
    QUERY->begin_class(QUERY, "TediumTrig", "UGen");
    QUERY->add_ctor(QUERY, tedtrig_ctor);
    QUERY->add_dtor(QUERY, tedtrig_dtor);
    QUERY->add_ugen_func(QUERY, tedtrig_tick, NULL, 0, 1);
    QUERY->add_mfun(QUERY, tedtrig_chan, "int", "select");
    QUERY->add_arg(QUERY, "int", "c");
    tedtrig_offset = QUERY->add_mvar(QUERY, "int", "@ted_trig", FALSE);
    QUERY->end_class(QUERY);

    /* ---- TediumGate ---- */
    QUERY->begin_class(QUERY, "TediumGate", "UGen");
    QUERY->add_ctor(QUERY, tedgate_ctor);
    QUERY->add_dtor(QUERY, tedgate_dtor);
    QUERY->add_ugen_func(QUERY, tedgate_tick, NULL, 1, 1);
    QUERY->add_mfun(QUERY, tedgate_chan, "int", "select");
    QUERY->add_arg(QUERY, "int", "c");
    tedgate_offset = QUERY->add_mvar(QUERY, "int", "@ted_gate", FALSE);
    QUERY->end_class(QUERY);

    /* ---- Tedium ---- */
    QUERY->begin_class(QUERY, "Tedium", "Object");
    QUERY->add_ctor(QUERY, tedctl_ctor);
    QUERY->add_dtor(QUERY, tedctl_dtor);

    QUERY->add_mfun(QUERY, tedctl_cv, "float", "cv");
    QUERY->add_arg(QUERY, "int", "c");

    QUERY->add_mfun(QUERY, tedctl_trig, "int", "trig");
    QUERY->add_arg(QUERY, "int", "c");

    QUERY->add_mfun(QUERY, tedctl_button, "int", "button");
    QUERY->add_arg(QUERY, "int", "c");

    QUERY->add_mfun(QUERY, tedctl_hold, "float", "hold");
    QUERY->add_arg(QUERY, "int", "c");

    QUERY->add_mfun(QUERY, tedctl_gate, "int", "gate");
    QUERY->add_arg(QUERY, "int", "c");
    QUERY->add_arg(QUERY, "int", "v");

    QUERY->add_mfun(QUERY, tedctl_smooth, "float", "smooth");
    QUERY->add_arg(QUERY, "float", "hz");

    QUERY->add_mfun(QUERY, tedctl_numcv, "int", "numCV");

    tedctl_offset = QUERY->add_mvar(QUERY, "int", "@ted_ctl", FALSE);
    QUERY->end_class(QUERY);

    return TRUE;
}
