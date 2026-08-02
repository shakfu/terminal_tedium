/*
 * libtedium - host-agnostic I/O layer for the Terminal Tedium eurorack module.
 *
 * The library owns the MCP3208 CV inputs, the GPIO trigger inputs, the gate
 * outputs and the panel buttons. It knows nothing about any audio engine.
 * Pure Data, Csound, ChucK, JACK and plain C/C++ hosts all consume the same
 * API through thin bindings in bindings/.
 *
 * Threading model
 * ---------------
 *   producer  a SCHED_FIFO thread owned by the library scans the ADC at a
 *             fixed rate and drains kernel GPIO edge events.
 *   consumer  the host's audio callback. Every tt_* function documented as
 *             "audio thread" is wait-free: no locks, no allocation, no
 *             syscalls.
 *
 * There is exactly one consumer. Calling the audio-thread functions from two
 * threads at once is undefined.
 *
 * Timebase
 * --------
 * All times are CLOCK_MONOTONIC nanoseconds (mach_absolute_time converted to
 * ns on Darwin). The consumer asks for CV "as of" a given instant, so a host
 * that knows the exact hardware time of its block start gets sample-accurate
 * CV. Hosts that do not know it can derive a good estimate with tt_timebase.
 *
 * Units
 * -----
 * CV is reported in volts after calibration, nominally -5.0 to +5.0. The
 * hardware's polarity inversion and the panel-to-ADC channel mapping are
 * applied inside the library; callers see channel 0 as the leftmost panel
 * input on both board variants.
 */

#ifndef TEDIUM_H
#define TEDIUM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TT_VERSION_MAJOR 0
#define TT_VERSION_MINOR 1
#define TT_VERSION_PATCH 0

#define TT_MAX_CV       8
#define TT_MAX_TRIGGERS 4
#define TT_MAX_GATES    2
#define TT_MAX_BUTTONS  3

/* Longest error string tt_open may write, including the terminator. */
#define TT_ERRLEN 256

/* ------------------------------------------------------------------ */
/* board variants                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    TT_BOARD_WM8731 = 0,   /* 6 CV in, 2 gate out on GPIO 12/16 */
    TT_BOARD_PCM5102A = 1  /* 8 CV in, 2 gate out on GPIO 16/26 */
} tt_board;

/* Number of CV channels the given board exposes on its panel. */
int tt_board_num_cv(tt_board board);

/* Human readable board name, never NULL. */
const char *tt_board_name(tt_board board);

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    TT_HAL_AUTO = 0,  /* linux on Linux, sim elsewhere */
    TT_HAL_LINUX,     /* spidev + libgpiod v2; fails if unavailable */
    TT_HAL_SIM        /* synthesised input, for development and tests */
} tt_hal_kind;

typedef struct {
    tt_board    board;
    tt_hal_kind hal;

    /* ADC scan rate in Hz. The library samples every CV channel this many
     * times per second and interpolates up to the host's sample rate.
     * 0 selects the default (4000). Above ~16000 the syscall cost per scan
     * starts to dominate a Pi core; measure with tools/tedium-bench. */
    int scan_rate_hz;

    /* SPI clock. 0 selects 4000000. The MCP3208 tolerates 2 MHz at 2.7 V and
     * 4 MHz at 5 V; going faster than the part is rated for produces quiet
     * bit errors rather than an obvious failure. */
    int spi_speed_hz;

    /* SCHED_FIFO priority for the sampling thread, 1..99. 0 leaves the thread
     * at normal priority, which is the right choice when not running on a
     * PREEMPT_RT kernel or without CAP_SYS_NICE. */
    int rt_priority;

    /* Device paths. NULL selects the platform default:
     *   spi_dev   "/dev/spidev0.1"
     *   gpio_chip "/dev/gpiochip0"  (gpiochip4 on Pi 5, autodetected) */
    const char *spi_dev;
    const char *gpio_chip;

    /* Calibration file. NULL selects $XDG_CONFIG_HOME/terminal_tedium/cal.json
     * falling back to ~/.config/terminal_tedium/cal.json. A missing file is
     * not an error; the library falls back to nominal scaling. */
    const char *cal_path;

    /* Depth of the CV history ring in scans. Rounded up to a power of two.
     * 0 selects 1024, which at the default scan rate is 256 ms of history --
     * far more than any sane block size needs, and the margin is what keeps
     * the reader from being lapped by the writer. */
    int ring_scans;
} tt_config;

/* Fill cfg with defaults for the given board. Always call this before setting
 * individual fields, so that fields added in later versions stay sane. */
void tt_config_init(tt_config *cfg, tt_board board);

/* ------------------------------------------------------------------ */
/* context                                                             */
/* ------------------------------------------------------------------ */

typedef struct tt_ctx tt_ctx;

/* Open the hardware and start the sampling thread.
 * Returns NULL on failure and writes a human readable reason into err, which
 * must be at least TT_ERRLEN bytes (err may be NULL). */
tt_ctx *tt_open(const tt_config *cfg, char *err, size_t errlen);

/* Stop the sampling thread, release the hardware, free the context.
 * Safe to call with NULL. Gates are driven low before the GPIO is released. */
void tt_close(tt_ctx *ctx);

/* Number of CV channels this context reports. */
int tt_num_cv(const tt_ctx *ctx);

/* Actual scan rate in Hz, which may differ from the requested one. */
int tt_scan_rate(const tt_ctx *ctx);

/* ------------------------------------------------------------------ */
/* CV input -- audio thread                                            */
/* ------------------------------------------------------------------ */

/* Render one block of CV per channel, interpolated onto the host's sample
 * grid. This is the function signal-rate hosts should use.
 *
 *   t0_ns        monotonic time of the first frame in the block
 *   ns_per_frame 1e9 / sample_rate
 *   nframes      frames to write
 *   out          out[ch] receives nframes floats, in volts.
 *                out[ch] may be NULL to skip that channel.
 *   nchan        length of out; channels beyond tt_num_cv() are zeroed.
 *
 * Wait-free. Returns 0 on success, or -1 if the context has no data yet, in
 * which case every requested buffer is filled with the last known value (or
 * zero). Callers can safely ignore the return value.
 */
int tt_cv_block(tt_ctx *ctx, uint64_t t0_ns, double ns_per_frame,
                uint32_t nframes, float *const *out, int nchan);

/* Latest CV values, for control-rate hosts that do not want a block.
 * Writes min(nchan, tt_num_cv()) volts into out. Wait-free. */
int tt_cv_latest(tt_ctx *ctx, float *out, int nchan);

/* Raw uncalibrated 12-bit codes, panel-ordered, for calibration tooling and
 * diagnostics. Wait-free. */
int tt_cv_raw(tt_ctx *ctx, uint16_t *out, int nchan);

/* One-pole smoothing applied to tt_cv_block output, in Hz. 0 disables it.
 * Unlike the legacy `smooth' message this filters across scans, so it
 * actually attenuates the low-frequency noise that makes knobs jitter.
 * Default 0. */
void tt_set_cv_smoothing(tt_ctx *ctx, float cutoff_hz);

/* ------------------------------------------------------------------ */
/* events -- audio thread                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    TT_EV_TRIGGER = 0,  /* index 0..3, a jack on the panel  */
    TT_EV_BUTTON  = 1   /* index 0..2, a tact switch        */
} tt_event_type;

typedef struct {
    uint64_t time_ns;   /* kernel edge timestamp, CLOCK_MONOTONIC */
    uint8_t  type;      /* tt_event_type */
    uint8_t  index;
    uint8_t  value;     /* 1 = trigger high / button pressed, 0 = released.
                         * Electrical polarity is normalised by the library:
                         * the inputs are pulled up and pull low when active,
                         * but callers always see active == 1. */
    uint8_t  _pad;
} tt_event;

/* Drain up to max events into buf, oldest first. Returns the count.
 * Events are timestamped by the kernel at the edge, so a host can place them
 * at an exact sample offset:
 *
 *     offset = (int)((ev.time_ns - t0_ns) / ns_per_frame);
 *
 * Edges that arrived during the previous block yield a negative offset; clamp
 * to 0. Wait-free. */
int tt_poll_events(tt_ctx *ctx, tt_event *buf, int max);

/*
 * Render the trigger inputs as gate signals, with each edge placed on the
 * exact sample the kernel timestamped it.
 *
 * This is the function that turns a 1.3 ms-jittered, occasionally-dropped
 * trigger into a sample-accurate one, and it is why the library exists.
 *
 *   out[ch][f] is 1.0 while trigger ch is active, 0.0 otherwise.
 *
 * The window rendered is [t0_ns, t0_ns + nframes*ns_per_frame). Edges cannot
 * be placed inside a window that has not happened yet, so callers must pass a
 * t0 that lags real time by at least one block:
 *
 *     uint64_t block_start = tt_timebase_tick(&tb, tt_now_ns(), nframes);
 *     uint64_t render_t0   = block_start - (uint64_t)(nframes * ns_per_frame);
 *
 * Pass that same render_t0 to tt_cv_block so CV and gates stay aligned. The
 * cost is one block of constant latency; constant latency is musically
 * harmless in a way that jitter is not.
 *
 * An edge older than the window is placed at frame 0 rather than dropped.
 * Wait-free. Independent of tt_poll_events -- using both is fine.
 */
int tt_trigger_block(tt_ctx *ctx, uint64_t t0_ns, double ns_per_frame,
                     uint32_t nframes, float *const *out, int nchan);

/*
 * Drive the gate outputs from signals. A crossing of 0.5 anywhere in the
 * block is scheduled for the instant it occurs, so output timing is bounded
 * by the scan period (250 us at the default rate) rather than by the host's
 * block size. in[ch] may be NULL to leave that gate alone. Wait-free.
 */
int tt_gate_block(tt_ctx *ctx, uint64_t t0_ns, double ns_per_frame,
                  uint32_t nframes, const float *const *in, int nchan);

/* Current debounced level of a trigger input (1 = active). Wait-free. */
int tt_trigger_state(tt_ctx *ctx, int index);

/* Current button level (1 = pressed), and how long it has been held in
 * milliseconds (0 when not held). Wait-free. */
int tt_button_state(tt_ctx *ctx, int index);
double tt_button_held_ms(tt_ctx *ctx, int index);

/* ------------------------------------------------------------------ */
/* gate output -- audio thread                                         */
/* ------------------------------------------------------------------ */

/* Set gate index (0..TT_MAX_GATES-1) high or low. The write is handed to the
 * sampling thread and lands within one scan period, so at the default scan
 * rate output timing is quantised to 250 us regardless of the host's block
 * size. Wait-free. */
int tt_gate_set(tt_ctx *ctx, int index, int on);

/* Schedule a gate change for a specific time. Use this to preserve the timing
 * of an event the host resolved to a sample offset. Times in the past fire
 * immediately; at most one pending change per gate is queued. Wait-free. */
int tt_gate_set_at(tt_ctx *ctx, int index, int on, uint64_t time_ns);

/* ------------------------------------------------------------------ */
/* calibration                                                         */
/* ------------------------------------------------------------------ */

/* Per channel: volts = (raw_code - offset) * scale.
 * Nominal values are offset = 2048, scale = -10.0/4096 (the hardware inverts).
 * A real board needs measured values; see tools/tedium-cal. */
typedef struct {
    float offset;
    float scale;
} tt_cal_channel;

typedef struct {
    tt_cal_channel ch[TT_MAX_CV];
} tt_cal;

/* Nominal calibration for a board, used when no file is present. */
void tt_cal_nominal(tt_cal *cal, tt_board board);

/* Read and write the calibration held by a live context. Not audio-thread
 * safe; call from the control thread. */
void tt_cal_get(const tt_ctx *ctx, tt_cal *out);
void tt_cal_set(tt_ctx *ctx, const tt_cal *cal);

/* JSON persistence. Return 0 on success, -1 on failure (errno is set for I/O
 * errors). Both work without a context so tooling can use them standalone. */
int tt_cal_load_file(tt_cal *cal, const char *path);
int tt_cal_save_file(const tt_cal *cal, const char *path);

/* Default calibration path for this user. Writes at most len bytes including
 * the terminator; returns the length that would have been written. */
size_t tt_cal_default_path(char *buf, size_t len);

/* ------------------------------------------------------------------ */
/* timebase helper                                                     */
/* ------------------------------------------------------------------ */

/*
 * JACK reports the exact hardware time of each cycle, so a JACK host can pass
 * it straight to tt_cv_block. Pd, Csound and ChucK do not expose anything
 * equivalent, and reading the clock at the top of the callback is far too
 * noisy to place events against -- the jitter of the callback itself lands
 * directly on every trigger.
 *
 * tt_timebase is a first-order delay-locked loop over the callback arrival
 * times. It converges on the true block period and returns a smoothed
 * estimate of the block start, which is what makes sub-block event placement
 * possible on hosts that cannot tell us the truth.
 */
typedef struct {
    double   period_ns;   /* filtered estimate of one block, in ns */
    double   t_next;      /* filtered estimate of the next block start */
    double   b, c;        /* DLL loop coefficients */
    uint64_t frames;      /* frames elapsed since init */
    int      warmed;
} tt_timebase;

/* bandwidth_hz sets how fast the loop tracks; 0 selects a sensible default.
 * Lower is smoother and slower to converge. */
void tt_timebase_init(tt_timebase *tb, double sample_rate, uint32_t nframes,
                      double bandwidth_hz);

/* Call once at the top of every audio callback, passing tt_now_ns().
 * Returns the estimated monotonic time of the block's first frame. */
uint64_t tt_timebase_tick(tt_timebase *tb, uint64_t now_ns, uint32_t nframes);

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

/* CLOCK_MONOTONIC now, in nanoseconds. Wait-free on Linux and Darwin. */
uint64_t tt_now_ns(void);

/* Library version string, e.g. "0.1.0". */
const char *tt_version(void);

/* Name of the HAL backend actually in use ("linux" or "sim"). */
const char *tt_hal_name(const tt_ctx *ctx);

/* Diagnostics, for the bench tool and for reporting xruns to the user.
 * Not audio-thread safe. */
typedef struct {
    uint64_t scans;            /* completed ADC scans */
    uint64_t scan_overruns;    /* scans that missed their deadline */
    uint64_t events;           /* GPIO edges delivered */
    uint64_t events_dropped;   /* edges lost to a full event ring */
    double   scan_us_mean;     /* mean wall time of one ADC scan */
    double   scan_us_max;      /* worst observed */
} tt_stats;

void tt_get_stats(const tt_ctx *ctx, tt_stats *out);
void tt_reset_stats(tt_ctx *ctx);

/* ------------------------------------------------------------------ */
/* simulation control (TT_HAL_SIM only)                                */
/* ------------------------------------------------------------------ */

/* Drive a simulated CV channel to a fixed value in volts. */
void tt_sim_set_cv(tt_ctx *ctx, int channel, float volts);

/* Inject a simulated edge. Returns 0 on success, -1 if not a sim context. */
int tt_sim_event(tt_ctx *ctx, tt_event_type type, int index, int value);

/* Read back the current level of a simulated gate output, for tests. */
int tt_sim_gate_state(tt_ctx *ctx, int index);

#ifdef __cplusplus
}
#endif

#endif /* TEDIUM_H */
