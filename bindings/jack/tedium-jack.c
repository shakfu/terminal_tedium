/*
 * tedium-jack - exposes the module's CV, triggers and gates as JACK ports.
 *
 * This is the binding that scales to more than one engine. Instead of
 * writing an external, an opcode and a chugin for every host, run this once
 * and every JACK client on the machine sees:
 *
 *   tedium:cv1 .. cvN     audio out, one volt per unit (so -5.0 .. +5.0)
 *   tedium:trig1 .. trig4 audio out, 1.0 while the input is high
 *   tedium:gate1, gate2   audio in, threshold 0.5
 *   tedium:buttons        MIDI out, note 0/1/2 on channel 1
 *
 * Csound, ChucK, Pd, SuperCollider and any C or C++ program then treat CV as
 * ordinary audio channels, which they all already know how to do. No
 * host-specific code, no per-engine SDK, and CV that can be patched straight
 * into any audio-rate parameter.
 *
 * The client runs first in the graph, so a consumer reading tedium:cv1 in
 * cycle N gets CV sampled for cycle N. There is no extra buffer of latency
 * beyond the deliberate one-block lag that makes edges land exactly.
 *
 * JACK is the one host that reports real hardware cycle times, so this
 * binding does not need the DLL timebase the others rely on.
 */

#include "tedium/tedium.h"

#include <jack/jack.h>
#include <jack/midiport.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    tt_ctx      *tt;
    jack_client_t *client;

    jack_port_t *cv[TT_MAX_CV];
    jack_port_t *trig[TT_MAX_TRIGGERS];
    jack_port_t *gate[TT_MAX_GATES];
    jack_port_t *midi;

    int          ncv;
    double       sr;

    /* Button levels, so the MIDI port emits changes rather than a stream. */
    unsigned     btn_state;

    /* Fallback timebase, used only if jack_get_cycle_times fails. */
    tt_timebase  tb;
    int          use_dll;
} tj;

static volatile sig_atomic_t g_quit;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ------------------------------------------------------------------ */

static int process(jack_nframes_t nframes, void *arg)
{
    tj *s = (tj *)arg;
    float *cvbuf[TT_MAX_CV];
    float *trigbuf[TT_MAX_TRIGGERS];
    const float *gatebuf[TT_MAX_GATES];
    double ns_per_frame = 1e9 / s->sr;
    uint64_t block_start, render_t0;
    void *midibuf;
    int i;

    for (i = 0; i < s->ncv; i++)
        cvbuf[i] = (float *)jack_port_get_buffer(s->cv[i], nframes);
    for (i = 0; i < TT_MAX_TRIGGERS; i++)
        trigbuf[i] = (float *)jack_port_get_buffer(s->trig[i], nframes);
    for (i = 0; i < TT_MAX_GATES; i++)
        gatebuf[i] = (const float *)jack_port_get_buffer(s->gate[i], nframes);

    /* JACK knows the real hardware time of this cycle, which is exactly what
     * the other bindings have to estimate. Use it when available. */
    if (!s->use_dll) {
        jack_nframes_t cur_frames;
        jack_time_t cur_usecs, next_usecs;
        float period_usecs;

        if (jack_get_cycle_times(s->client, &cur_frames, &cur_usecs,
                                 &next_usecs, &period_usecs) == 0) {
            block_start = (uint64_t)cur_usecs * 1000ull;
        } else {
            /* Fall back once and stay fallen back; flapping between two
             * timebases would be worse than either. */
            s->use_dll = 1;
            tt_timebase_init(&s->tb, s->sr, nframes, 0.5);
            block_start = tt_now_ns();
        }
    } else {
        block_start = tt_timebase_tick(&s->tb, tt_now_ns(), nframes);
    }

    render_t0 = block_start - (uint64_t)(ns_per_frame * (double)nframes);

    tt_cv_block(s->tt, render_t0, ns_per_frame, nframes, cvbuf, s->ncv);
    tt_trigger_block(s->tt, render_t0, ns_per_frame, nframes,
                     trigbuf, TT_MAX_TRIGGERS);
    tt_gate_block(s->tt, block_start, ns_per_frame, nframes,
                  gatebuf, TT_MAX_GATES);

    /* Buttons as MIDI notes, placed at their timestamped sample. */
    midibuf = jack_port_get_buffer(s->midi, nframes);
    jack_midi_clear_buffer(midibuf);
    {
        tt_event ev[32];
        int n = tt_poll_events(s->tt, ev, 32);
        for (i = 0; i < n; i++) {
            jack_nframes_t off;
            jack_midi_data_t *m;

            if (ev[i].type != TT_EV_BUTTON) continue;

            if (ev[i].time_ns <= render_t0) {
                off = 0;
            } else {
                double d = (double)(ev[i].time_ns - render_t0) / ns_per_frame;
                off = (d >= (double)nframes) ? nframes - 1 : (jack_nframes_t)d;
            }

            m = jack_midi_event_reserve(midibuf, off, 3);
            if (!m) continue;
            m[0] = (jack_midi_data_t)(ev[i].value ? 0x90 : 0x80);
            m[1] = (jack_midi_data_t)ev[i].index;
            m[2] = (jack_midi_data_t)(ev[i].value ? 127 : 0);
        }
    }

    return 0;
}

static int on_srate(jack_nframes_t rate, void *arg)
{
    tj *s = (tj *)arg;
    s->sr = (double)rate;
    return 0;
}

static int on_bufsize(jack_nframes_t nframes, void *arg)
{
    tj *s = (tj *)arg;
    if (s->use_dll) tt_timebase_init(&s->tb, s->sr, nframes, 0.5);
    return 0;
}

static void on_shutdown(void *arg)
{
    (void)arg;
    g_quit = 1;
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-b wm8731|pcm5102a] [-r scan_hz] [-p rt_prio] [-c cal]\n"
        "          [-n client_name] [-s] [-a]\n"
        "  -s  simulation backend (for testing without the module)\n"
        "  -a  auto-connect CV outputs to the first client that will take them\n",
        argv0);
}

int main(int argc, char **argv)
{
    tt_config cfg;
    tj s;
    char err[TT_ERRLEN];
    char name[64];
    const char *client_name = "tedium";
    jack_status_t status;
    int i, autoconnect = 0;

    memset(&s, 0, sizeof(s));
    tt_config_init(&cfg, TT_BOARD_WM8731);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "pcm5102a")) cfg.board = TT_BOARD_PCM5102A;
            else if (!strcmp(argv[i], "wm8731")) cfg.board = TT_BOARD_WM8731;
            else { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            cfg.scan_rate_hz = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            cfg.rt_priority = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            cfg.cal_path = argv[++i];
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            client_name = argv[++i];
        } else if (!strcmp(argv[i], "-s")) {
            cfg.hal = TT_HAL_SIM;
        } else if (!strcmp(argv[i], "-a")) {
            autoconnect = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    s.tt = tt_open(&cfg, err, sizeof(err));
    if (!s.tt) {
        fprintf(stderr, "tedium: %s\n", err);
        return 1;
    }
    s.ncv = tt_num_cv(s.tt);

    s.client = jack_client_open(client_name, JackNoStartServer, &status);
    if (!s.client) {
        fprintf(stderr, "could not connect to JACK (status 0x%x). "
                        "Is jackd running?\n", (unsigned)status);
        tt_close(s.tt);
        return 1;
    }

    s.sr = (double)jack_get_sample_rate(s.client);
    jack_set_process_callback(s.client, process, &s);
    jack_set_sample_rate_callback(s.client, on_srate, &s);
    jack_set_buffer_size_callback(s.client, on_bufsize, &s);
    jack_on_shutdown(s.client, on_shutdown, &s);

    for (i = 0; i < s.ncv; i++) {
        snprintf(name, sizeof(name), "cv%d", i + 1);
        s.cv[i] = jack_port_register(s.client, name, JACK_DEFAULT_AUDIO_TYPE,
                                     JackPortIsOutput, 0);
    }
    for (i = 0; i < TT_MAX_TRIGGERS; i++) {
        snprintf(name, sizeof(name), "trig%d", i + 1);
        s.trig[i] = jack_port_register(s.client, name, JACK_DEFAULT_AUDIO_TYPE,
                                       JackPortIsOutput, 0);
    }
    for (i = 0; i < TT_MAX_GATES; i++) {
        snprintf(name, sizeof(name), "gate%d", i + 1);
        s.gate[i] = jack_port_register(s.client, name, JACK_DEFAULT_AUDIO_TYPE,
                                       JackPortIsInput, 0);
    }
    s.midi = jack_port_register(s.client, "buttons", JACK_DEFAULT_MIDI_TYPE,
                                JackPortIsOutput, 0);

    for (i = 0; i < s.ncv; i++)
        if (!s.cv[i]) { fprintf(stderr, "port registration failed\n"); goto out; }

    if (jack_activate(s.client) != 0) {
        fprintf(stderr, "could not activate JACK client\n");
        goto out;
    }

    printf("tedium-jack %s: %s board, %s backend, %d CV, scan %d Hz, "
           "JACK %.0f Hz\n",
           tt_version(), tt_board_name(cfg.board), tt_hal_name(s.tt),
           s.ncv, tt_scan_rate(s.tt), s.sr);
    printf("ports: %s:cv1..cv%d, trig1..trig4, gate1..gate2, buttons\n",
           client_name, s.ncv);

    if (autoconnect) {
        const char **targets = jack_get_ports(s.client, NULL,
                                              JACK_DEFAULT_AUDIO_TYPE,
                                              JackPortIsInput);
        if (targets) {
            for (i = 0; i < s.ncv && targets[i]; i++)
                jack_connect(s.client, jack_port_name(s.cv[i]), targets[i]);
            jack_free(targets);
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_quit) {
        tt_stats st;
        sleep(5);
        tt_get_stats(s.tt, &st);
        if (st.scan_overruns > st.scans / 20)
            fprintf(stderr, "tedium: %llu overruns in %llu scans -- lower "
                    "the scan rate or raise rt priority\n",
                    (unsigned long long)st.scan_overruns,
                    (unsigned long long)st.scans);
        if (st.events_dropped)
            fprintf(stderr, "tedium: %llu GPIO events dropped\n",
                    (unsigned long long)st.events_dropped);
    }

    printf("\nshutting down\n");
    jack_deactivate(s.client);

out:
    jack_client_close(s.client);
    tt_close(s.tt);
    return 0;
}
