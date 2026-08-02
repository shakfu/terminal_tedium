/*
 * Csound opcodes for libtedium.
 *
 *   acv1 [, acv2 ...]  tedcv                  ; CV as a-rate signals, volts
 *   kcv1 [, kcv2 ...]  tedcvk                 ; CV at k-rate, volts
 *   at1, at2, at3, at4 tedtrig                ; trigger inputs, a-rate gates
 *                      tedgate  agate1, agate2 ; drive the gate outputs
 *   kv                 tedbutton kindex       ; button level
 *   kms                tedhold   kindex       ; how long it has been held, ms
 *
 * Every opcode shares one hardware context, opened on first use and closed
 * when Csound unloads the module. The board variant and scan rate come from
 * the environment so that an orchestra stays portable:
 *
 *   TEDIUM_BOARD=pcm5102a   TEDIUM_SCAN_HZ=8000   TEDIUM_RT_PRIO=70
 *   TEDIUM_HAL=sim          TEDIUM_CAL=/path/cal.json
 *
 * Csound gives no hardware timestamp for a control period, so the DLL
 * timebase recovers one; without it the jitter of Csound's own scheduling
 * would land on every trigger edge.
 */

#include "csdl.h"
#include "tedium/tedium.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* shared context                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    tt_ctx     *ctx;
    int         refs;
    int         ncv;
    tt_timebase tb;
    int         tb_inited;
    uint32_t    tb_nframes;
} ted_global;

static const char *g_key = "::tedium::global";

static ted_global *ted_get(CSOUND *csound)
{
    ted_global *g = (ted_global *)csound->QueryGlobalVariable(csound, g_key);
    if (g) return g;

    if (csound->CreateGlobalVariable(csound, g_key, sizeof(ted_global)) != 0)
        return NULL;
    g = (ted_global *)csound->QueryGlobalVariable(csound, g_key);
    return g;
}

static int env_int(CSOUND *csound, const char *name, int dflt)
{
    const char *v = csound->GetEnv(csound, name);
    return (v && *v) ? atoi(v) : dflt;
}

static int ted_reset(CSOUND *csound, void *userData)
{
    ted_global *g = (ted_global *)csound->QueryGlobalVariable(csound, g_key);
    (void)userData;
    if (g && g->ctx) {
        tt_close(g->ctx);
        g->ctx = NULL;
        g->refs = 0;
        g->tb_inited = 0;
    }
    return 0;
}

static tt_ctx *ted_open(CSOUND *csound)
{
    ted_global *g = ted_get(csound);
    tt_config cfg;
    char err[TT_ERRLEN];
    const char *s;

    if (!g) return NULL;
    if (g->ctx) { g->refs++; return g->ctx; }

    tt_config_init(&cfg, TT_BOARD_WM8731);

    s = csound->GetEnv(csound, "TEDIUM_BOARD");
    if (s && !strcmp(s, "pcm5102a")) cfg.board = TT_BOARD_PCM5102A;

    s = csound->GetEnv(csound, "TEDIUM_HAL");
    if (s && !strcmp(s, "sim")) cfg.hal = TT_HAL_SIM;

    cfg.scan_rate_hz = env_int(csound, "TEDIUM_SCAN_HZ", 4000);
    cfg.rt_priority  = env_int(csound, "TEDIUM_RT_PRIO", 0);

    s = csound->GetEnv(csound, "TEDIUM_CAL");
    if (s && *s) cfg.cal_path = s;

    g->ctx = tt_open(&cfg, err, sizeof(err));
    if (!g->ctx) {
        csound->Message(csound, "tedium: %s\n", err);
        return NULL;
    }
    g->ncv = tt_num_cv(g->ctx);
    g->refs = 1;

    /* LINKAGE only exports csound_opcode_init, so csoundModuleDestroy is
     * never called for an opcode plugin. Without a reset callback the
     * sampling thread outlives Csound's globals and faults on exit. */
    csound->RegisterResetCallback(csound, NULL, ted_reset);

    csound->Message(csound, "tedium: %s board, %s backend, %d CV, %d Hz scan\n",
                    tt_board_name(cfg.board), tt_hal_name(g->ctx),
                    g->ncv, tt_scan_rate(g->ctx));
    return g->ctx;
}

/*
 * One timebase tick per control period, shared by every opcode instance, so
 * that CV and triggers in the same k-cycle refer to the same instant.
 * Csound calls opcodes in order within a period; whichever runs first
 * advances the clock and the rest reuse it.
 */
static uint64_t ted_block_time(CSOUND *csound, ted_global *g, uint32_t nframes)
{
    if (!g->tb_inited || g->tb_nframes != nframes) {
        tt_timebase_init(&g->tb, csound->GetSr(csound), nframes, 0.5);
        g->tb_inited = 1;
        g->tb_nframes = nframes;
    }
    return tt_timebase_tick(&g->tb, tt_now_ns(), nframes);
}

/* ------------------------------------------------------------------ */
/* tedcv : a-rate CV                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    OPDS   h;
    MYFLT *out[TT_MAX_CV];
} TEDCV;

static int32_t tedcv_init(CSOUND *csound, TEDCV *p)
{
    if (!ted_open(csound))
        return csound->InitError(csound, "%s", "tedcv: could not open device");
    return OK;
}

static int32_t tedcv_perf(CSOUND *csound, TEDCV *p)
{
    ted_global *g = ted_get(csound);
    uint32_t nsmps = CS_KSMPS;
    int nout = (int)p->OUTOCOUNT;
    float tmp[TT_MAX_CV][64];
    float *ptr[TT_MAX_CV];
    uint64_t t0, render;
    double ns_per_frame;
    uint32_t n, done, i;
    int ch;

    if (!g || !g->ctx) return OK;
    if (nout > TT_MAX_CV) nout = TT_MAX_CV;

    ns_per_frame = 1e9 / csound->GetSr(csound);
    t0     = ted_block_time(csound, g, nsmps);
    render = t0 - (uint64_t)(ns_per_frame * (double)nsmps);

    /* libtedium works in float; Csound's MYFLT may be double, and ksmps may
     * exceed the scratch buffer, so render in chunks. */
    for (done = 0; done < nsmps; done += n) {
        n = nsmps - done;
        if (n > 64) n = 64;

        for (ch = 0; ch < nout; ch++) ptr[ch] = tmp[ch];
        tt_cv_block(g->ctx, render + (uint64_t)(ns_per_frame * (double)done),
                    ns_per_frame, n, ptr, nout);

        for (ch = 0; ch < nout; ch++)
            for (i = 0; i < n; i++)
                p->out[ch][done + i] = (MYFLT)tmp[ch][i];
    }
    return OK;
}

/* ------------------------------------------------------------------ */
/* tedcvk : k-rate CV                                                  */
/* ------------------------------------------------------------------ */

static int32_t tedcvk_perf(CSOUND *csound, TEDCV *p)
{
    ted_global *g = ted_get(csound);
    float v[TT_MAX_CV];
    int nout = (int)p->OUTOCOUNT;
    int ch;

    if (!g || !g->ctx) return OK;
    if (nout > TT_MAX_CV) nout = TT_MAX_CV;

    tt_cv_latest(g->ctx, v, TT_MAX_CV);
    for (ch = 0; ch < nout; ch++) *p->out[ch] = (MYFLT)v[ch];
    return OK;
}

/* ------------------------------------------------------------------ */
/* tedtrig : a-rate trigger inputs                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    OPDS   h;
    MYFLT *out[TT_MAX_TRIGGERS];
} TEDTRIG;

static int32_t tedtrig_init(CSOUND *csound, TEDTRIG *p)
{
    if (!ted_open(csound))
        return csound->InitError(csound, "%s", "tedtrig: could not open device");
    return OK;
}

static int32_t tedtrig_perf(CSOUND *csound, TEDTRIG *p)
{
    ted_global *g = ted_get(csound);
    uint32_t nsmps = CS_KSMPS;
    int nout = (int)p->OUTOCOUNT;
    float tmp[TT_MAX_TRIGGERS][64];
    float *ptr[TT_MAX_TRIGGERS];
    uint64_t t0, render;
    double ns_per_frame;
    uint32_t n, done, i;
    int ch;

    if (!g || !g->ctx) return OK;
    if (nout > TT_MAX_TRIGGERS) nout = TT_MAX_TRIGGERS;

    ns_per_frame = 1e9 / csound->GetSr(csound);
    t0     = ted_block_time(csound, g, nsmps);
    render = t0 - (uint64_t)(ns_per_frame * (double)nsmps);

    for (done = 0; done < nsmps; done += n) {
        n = nsmps - done;
        if (n > 64) n = 64;

        for (ch = 0; ch < nout; ch++) ptr[ch] = tmp[ch];
        tt_trigger_block(g->ctx,
                         render + (uint64_t)(ns_per_frame * (double)done),
                         ns_per_frame, n, ptr, nout);

        for (ch = 0; ch < nout; ch++)
            for (i = 0; i < n; i++)
                p->out[ch][done + i] = (MYFLT)tmp[ch][i];
    }
    return OK;
}

/* ------------------------------------------------------------------ */
/* tedgate : gate outputs                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    OPDS   h;
    MYFLT *in[TT_MAX_GATES];
} TEDGATE;

static int32_t tedgate_init(CSOUND *csound, TEDGATE *p)
{
    if (!ted_open(csound))
        return csound->InitError(csound, "%s", "tedgate: could not open device");
    return OK;
}

static int32_t tedgate_perf(CSOUND *csound, TEDGATE *p)
{
    ted_global *g = ted_get(csound);
    uint32_t nsmps = CS_KSMPS;
    int nin = (int)p->INOCOUNT;
    float tmp[TT_MAX_GATES][64];
    const float *ptr[TT_MAX_GATES];
    uint64_t t0;
    double ns_per_frame;
    uint32_t n, done, i;
    int ch;

    if (!g || !g->ctx) return OK;
    if (nin > TT_MAX_GATES) nin = TT_MAX_GATES;

    ns_per_frame = 1e9 / csound->GetSr(csound);
    t0 = ted_block_time(csound, g, nsmps);

    for (done = 0; done < nsmps; done += n) {
        n = nsmps - done;
        if (n > 64) n = 64;

        for (ch = 0; ch < nin; ch++) {
            for (i = 0; i < n; i++) tmp[ch][i] = (float)p->in[ch][done + i];
            ptr[ch] = tmp[ch];
        }
        tt_gate_block(g->ctx, t0 + (uint64_t)(ns_per_frame * (double)done),
                      ns_per_frame, n, ptr, nin);
    }
    return OK;
}

/* ------------------------------------------------------------------ */
/* tedbutton / tedhold                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    OPDS   h;
    MYFLT *out;
    MYFLT *index;
} TEDBTN;

static int32_t tedbtn_init(CSOUND *csound, TEDBTN *p)
{
    if (!ted_open(csound))
        return csound->InitError(csound, "%s", "tedbutton: could not open device");
    return OK;
}

static int32_t tedbutton_perf(CSOUND *csound, TEDBTN *p)
{
    ted_global *g = ted_get(csound);
    if (!g || !g->ctx) { *p->out = FL(0.0); return OK; }
    *p->out = (MYFLT)tt_button_state(g->ctx, (int)*p->index - 1);
    return OK;
}

static int32_t tedhold_perf(CSOUND *csound, TEDBTN *p)
{
    ted_global *g = ted_get(csound);
    if (!g || !g->ctx) { *p->out = FL(0.0); return OK; }
    *p->out = (MYFLT)tt_button_held_ms(g->ctx, (int)*p->index - 1);
    return OK;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

/*
 * OENTRY is { name, dsblksiz, flags, thread, outypes, intypes,
 *             iopadr, kopadr, aopadr }.
 *
 * thread 3 = init + k-rate, 5 = init + a-rate.
 * "m" is an indefinite run of a-rate outputs, "z" of k-rate outputs, which
 * is how one opcode serves 6 or 8 CV channels depending on the board.
 */
static OENTRY localops[] = {
    { "tedcv",     sizeof(TEDCV),   0, 5, "m", "",
      (SUBR)tedcv_init,   NULL,                 (SUBR)tedcv_perf   , NULL },
    { "tedcvk",    sizeof(TEDCV),   0, 3, "z", "",
      (SUBR)tedcv_init,   (SUBR)tedcvk_perf,    NULL               , NULL },
    { "tedtrig",   sizeof(TEDTRIG), 0, 5, "m", "",
      (SUBR)tedtrig_init, NULL,                 (SUBR)tedtrig_perf , NULL },
    { "tedgate",   sizeof(TEDGATE), 0, 5, "",  "aa",
      (SUBR)tedgate_init, NULL,                 (SUBR)tedgate_perf , NULL },
    { "tedbutton", sizeof(TEDBTN),  0, 3, "k", "k",
      (SUBR)tedbtn_init,  (SUBR)tedbutton_perf, NULL               , NULL },
    { "tedhold",   sizeof(TEDBTN),  0, 3, "k", "k",
      (SUBR)tedbtn_init,  (SUBR)tedhold_perf,   NULL               , NULL }
};

LINKAGE
