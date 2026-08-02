# Changelog

Notable changes to this fork of Terminal Tedium.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions refer to `libtedium` (`tt_version()`); the hardware is unchanged.

## [0.1.0] - 2026-08-02

First release of `libtedium`, a host-agnostic I/O layer that replaces the
original Pd externals. The motivation and the full assessment of the prior
state are in [REVIEW.md](REVIEW.md); the design is in
[ARCHITECTURE.md](ARCHITECTURE.md).

The short version: the module's ceiling was I/O timing, not CPU. CV updated at
100 Hz from blocking SPI calls made on Pd's own audio thread, and triggers were
polled from a 1 ms Pd clock that only fires at DSP block boundaries, so every
trigger carried up to 1.333 ms of jitter and sub-millisecond pulses were
dropped outright. Supporting a second audio engine would have meant writing the
whole driver again.

### Added

#### libtedium core

- Host-agnostic C11 library (`include/tedium/tedium.h`) with no audio-engine
  dependencies. A dedicated `SCHED_FIFO` sampling thread owns every syscall;
  the audio callback only reads memory. No SPI transfer can land on the audio
  thread any more.
- Timestamped CV history ring, kept as history rather than as a queue, so a
  consumer can ask for CV *as of an arbitrary instant*. This is what makes CV a
  signal rather than a sequence of control events.
- `tt_cv_block()` renders CV onto the host's sample grid by interpolating
  between bracketing scans. CV can now be patched straight into a filter cutoff
  with no zipper noise and no `[line~]` smoothing.
- `tt_trigger_block()` renders trigger inputs as gate signals with each edge
  placed on the sample the kernel timestamped it. Trigger placement improves
  from +/-1.333 ms to +/-1 sample (about 20 us), and short pulses are queued by
  the kernel rather than missed between polls.
- `tt_gate_block()` drives the gate outputs from signals, with output timing
  bounded by the scan period (250 us by default) instead of by the host's block
  size.
- `tt_timebase`, a second-order delay-locked loop over audio-callback arrival
  times, for the hosts that expose no hardware timestamp. Without it the
  callback's own scheduling jitter would land directly on every trigger edge.
- Per-channel calibration, `volts = (raw_code - offset) * scale`, persisted as
  JSON. Every binding reports volts rather than raw counts, so patches are
  portable and 1V/oct is just `pow(2, v)`.
- Board descriptors for both revisions. Channel 0 is the leftmost panel input
  on both, so nothing above the HAL is variant-aware.
- Diagnostics (`tt_get_stats`): scan count, overruns, event count, dropped
  events, and mean/max scan cost.

#### Hardware abstraction

- `linux` backend using spidev and the `/dev/gpiochip` character device
  (uapi v2 with a v1 fallback). Replaces wiringPi, which its author deprecated
  in 2019 and whose register-mmap approach cannot work on a Pi 5, where GPIO
  sits behind the RP1 southbridge.
- A six-channel ADC scan is now **one** `ioctl` (three-byte transfers with
  `cs_change` so the driver toggles chip select in hardware), where the legacy
  external issued one `ioctl` per byte per channel -- eighteen syscalls per
  scan.
- Trigger and button polarity is normalised in the HAL. The inputs are pulled
  up and pull low when active; callers always see active as 1.
- `sim` backend that synthesises input, so the library, the tools and the
  bindings build and run on a development machine with no module attached. It
  is a real backend, not a stub: the tests drive exact CV values and exact edge
  times through the real sampling thread, ring and interpolator.

#### Engine bindings

- `tedium-jack`: exposes CV, triggers and gates as JACK ports, and buttons as
  JACK MIDI. Any JACK client gets them with no engine-specific code. JACK is
  also the one host that reports true hardware cycle times, so this binding
  needs no DLL timebase.
- `[tedium~]` for Pure Data: one object replacing `terminal_tedium_adc`,
  `tedium_input`, `tedium_output` and `tedium_switch`. Signal inlets for gates,
  signal outlets for CV and triggers, a control outlet for buttons. One shared
  hardware context, refcounted, so multiple instances are safe.
- Csound opcodes: `tedcv`, `tedcvk`, `tedtrig`, `tedgate`, `tedbutton`,
  `tedhold`. Configured by environment so an orchestra stays portable.
- ChucK chugin: `TediumCV`, `TediumTrig`, `TediumGate` UGens plus a `Tedium`
  control class.
- Plain C and C++ programs can link `libtedium.a` and skip bindings entirely.

#### Tools

- `tedium-monitor`: live view of every input. The first thing to run on a new
  build; confirms SPI opens, GPIO lines are claimable, and the panel mapping
  and calibration are right.
- `tedium-cal`: solves the two-point calibration line from measured voltages
  and reports per-channel noise in cents of 1V/oct.
- `tedium-bench`: measures achieved scan rate, per-scan cost, duty cycle, CV
  noise floor, effective resolution, and whether oversampling actually buys
  resolution on your board. The review claimed averaging would recover about
  1.5 bits; this measures it rather than assuming it, since the gain only
  materialises if the noise is white and larger than one code.

#### Installer

- `install.sh` rewritten for current Raspberry Pi OS. The old one is retired to
  `attic/install.sh.2019` rather than fixed.
  - Writes `/boot/firmware/config.txt`. Since Bookworm, `/boot/config.txt` is a
    placeholder whose contents tell you not to edit it, so the old installer
    silently configured nothing: SPI and I2S were never enabled and the codec
    never appeared.
  - Edits inside a marked block, after a timestamped backup, so existing HDMI,
    KMS, wifi-country and overclock settings survive.
  - Opens the block with `[all]`. `config.txt` conditional filters (`[pi4]`,
    `[cm5]`, `[EDID=...]`) apply to everything after them, so a block appended
    to a file ending in a filter section would be scoped to that model only --
    settings that read correctly and never take effect. Covered by a
    regression test.
  - Uses `$SUDO_USER` and its real home. The `pi` user stopped being the
    default in Bullseye and does not exist on a fresh image.
  - Installs a systemd unit instead of `/etc/rc.local`, and leaves it disabled:
    starting hardware at boot should be an explicit choice.
  - Sets realtime limits in `/etc/security/limits.d` and adds the user to the
    groups that exist, so `rt_priority` can actually be granted.
  - Installs Pd and Csound from apt rather than fetching a 2018 tarball over
    plain HTTP with no checksum.
  - Runs the test suite before installing and refuses to continue if it fails.
  - Never deletes the repository. Never reboots without asking.
  - `--check` reports system state, `--dry-run` shows the plan, `--uninstall`
    reverses it, and re-running is idempotent.

#### Build and test

- A real build system. The legacy build was four inline `gcc`/`ld` pairs inside
  `install.sh`, which compiled without `-fPIC` and linked shared objects with
  `ld` directly.
- 80 unit tests requiring no hardware (`make test`), including assertions that
  an edge lands on its exact frame and that a sub-block pulse survives -- the
  two properties the whole rework rests on.
- `make test SAN=1` (AddressSanitizer, UBSan) and `make test TSAN=1`
  (ThreadSanitizer). All clean.
- `make test-integration` loads each binding in its real host and checks the
  values, catching the class of bug that compiling alone cannot: ABI
  mismatches, missing teardown hooks, method-name collisions.
- `make check-linux` parses the Linux HAL on a non-Linux machine against stub
  kernel uapi headers. Catches syntax and type errors in our own code; does not
  validate real struct layouts or ioctl numbers.
- A regression test for the installer's boot-config editing, which verifies the
  append/remove round-trip restores the file byte for byte and that the block is
  `[all]`-scoped. Written after that scoping bug was found in review.
- `make compile_commands` for editor and LSP support.
- `ARCHITECTURE.md`, `REVIEW.md`, and a `.gitignore`.

### Fixed

Defects in the original externals, which remain usable in `software/`:

- **`terminal_tedium_adc.c`: SPI descriptor leak on every object deletion.**
  The destructor guarded on `spifd == 0`, but descriptor 0 is stdin, so an open
  SPI device never matched. Enough patch reloads during a session and you
  exhaust the descriptor limit and can no longer open the ADC.
- **`terminal_tedium_adc.c`: `exit(1)` inside a Pd external.** A failed
  `close()` terminated the whole Pd process, and the instrument with it. Now
  reports through `pd_error()` and returns.
- **`OSC client/main.c`: undefined behaviour in the channel cycling.**
  `_cnt = _cnt++ >= ADC_NUM ? 0 : _cnt` modifies and reads `_cnt` with no
  sequence point between them. The cycling was also wrong independently of the
  UB.
- **`OSC client/main.c`: `memset(adc, 0, ADC_NUM_CHANNELS)`** cleared 6 of the
  array's 12 bytes. Now uses `sizeof`.
- **`OSC client/main.c`:** added the parentheses in `0x06 | (_channel>>2) & 0x01`
  that `-Wparentheses` flags. The README's own recommended `-Werror` build would
  have failed on it.
- **`adc_test.pd`: arbitrary board-variant selection.** `initbang` fanned out to
  both `open` and `open adc`, and Pd does not guarantee fan-out order, so which
  variant the object opened was effectively arbitrary -- silently putting a
  wm8731 board into 8-channel mode. The demo now sends only `open`, with a note
  explaining why both must never be sent.

### Changed

- **`software/externals/ReadMe.md`: corrected trigger GPIO documentation.** It
  listed the pcm5102a assignment for both revisions. That is wrong for wm8731
  boards, where GPIO 2 and 3 are the I2C lines; those boards use GPIO 4, 17, 14
  and 27, as the OSC client has always done. Users following the docs would
  have patched the wrong inputs.
- `README.md` now documents the library, the bindings and the build, and states
  plainly what is and is not verified.

### Removed

- `software/install.sh` moved to `attic/install.sh.2019`. See the Installer
  section above for why it is unsafe on a current image.
- `software/adc2FUDI.c` moved to `attic/`, not fixed. It was already dead code,
  referenced only from commented-out lines in `software/pdpd`, and superseded by
  the OSC client. It contained a two-byte heap overflow -- `malloc(msgLength)`
  followed by `snprintf(buf + 2, msgLength, ...)`, with the following `strlen()`
  depending on a NUL that could land outside the allocation -- and leaked the
  buffer on every message inside an infinite loop.

### Known limitations

- **`src/hal_linux.c` has never been compiled or run.** It parses against stub
  kernel headers only; no Linux machine or container was available. It needs to
  be built and exercised on the Pi before anything here can be called working
  on hardware.
- **The JACK binding compiles against stub headers only**, as JACK was not
  installed on the development machine.
- **Gate output is not sample-accurate.** Timing is bounded by the scan period
  rather than the host block, which is a large improvement, but
  `tt_gate_set_at()` keeps at most one pending change per gate: if a block
  contains several threshold crossings only the last survives. The final level
  is always correct; intermediate pulses closer together than a scan period are
  not represented.
- **Trigger and CV rendering costs one block of constant latency**, deliberately,
  so that edges can be placed exactly. Constant latency is musically harmless in
  a way that jitter is not, but it is not free.
- The Pi-side install has been rewritten but, like the Linux HAL, has only been
  exercised in `--dry-run` and `--check` on a non-Pi. Its boot-config editing is
  covered by a regression test; the rest awaits hardware.

## [0.0.0] - 2019-08-26

The upstream state this fork begins from (`d951dbf`): Pd externals over
wiringPi, an OSC client, and an installer pinned to Pd 0.48-1.
