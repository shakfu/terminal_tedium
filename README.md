terminal_tedium
===============

![My image](https://farm1.staticflickr.com/423/19280194146_4568770dcf_c.jpg)

raspberry pi 2 / pi 3 / zero (W) / pi a+ / b+  eurorack stereo codec (wm8731) breakout board *

- 4x digital inputs (> 100k input impedance; threshold > 3V)
- 2x digital outputs (~ 6V )
- 6x CV inputs (100k input impedance; 12 bit, +/- 5V range)
- 2x audio outputs (10VPP, 16bit / 48kHz)
- 2x audio inputs (50k input impedance, 16bit / 48kHz)

build info, see here: https://github.com/mxmxmx/terminal_tedium/wiki

[PCBs/panels](https://pushermanproductions.com/?s=terminal&post_type=product)

---

## libtedium

`libtedium` is a host-agnostic I/O layer for the module, replacing the original
Pd externals. It exists because the module's ceiling was I/O timing rather than
CPU, and because a driver locked inside a Pd external has to be rewritten for
every new audio engine.

What changes:

|  | before | now |
|---|---|---|
| CV rate | 100 Hz control-rate | audio-rate signal, interpolated from a 4 kHz scan |
| CV units | 0..4001 raw counts | calibrated volts |
| Trigger jitter | +/-1.333 ms (DSP block) | +/-1 sample (~20 us) |
| Short pulses | dropped below ~1.3 ms | kernel-queued, never dropped |
| SPI syscalls | on the audio thread | on a dedicated RT thread |
| Engines | Pd only | Pd, Csound, ChucK, JACK, or link the C API |

See [ARCHITECTURE.md](ARCHITECTURE.md) for how and why, and
[REVIEW.md](REVIEW.md) for the assessment that prompted it.

### Build

```sh
make            # library and tools
make test       # 80 tests, no hardware needed
make bindings   # every engine binding whose SDK is installed
```

The core, the tools, the tests and the Pd/Csound/ChucK bindings all build on
macOS against the simulation backend, so development does not need the module
attached. On the Pi, the Linux backend is selected automatically.

```sh
make test SAN=1         # AddressSanitizer + UBSan
make test TSAN=1        # ThreadSanitizer (the one that matters here)
make check-linux        # parse the Linux HAL on a non-Linux machine
make bindings && make test-integration   # load each binding in its real host
```

### Installing on the Pi

```sh
./install.sh --check        # report system state, change nothing
./install.sh --dry-run      # show the plan
./install.sh                # do it, with prompts
./install.sh --uninstall    # reverse it
```

Configures `/boot/firmware/config.txt` inside a marked block (after a backup),
sets realtime limits, builds and tests the library, and installs a systemd unit
that it leaves disabled. It never deletes the repository and never reboots
without asking. `--board pcm5102a` selects the other revision.

The 2019 installer is retired to `attic/`; it should not be run on a current
image, where it writes a path that no longer takes effect.

### First run on the module

```sh
./build/tedium-monitor          # live view of every input
./build/tedium-cal              # measure and store CV calibration
./build/tedium-bench            # scan timing, noise floor, ENOB
```

Run `tedium-cal` before trusting any pitch CV: at 2.44 mV per code, one code is
about 1.76 cents of 1V/oct, and uncalibrated channels will be tens of cents out
and mismatched with each other.

### Using it from an engine

**JACK** — the option that needs no engine-specific code. CV, triggers and gates
become ordinary JACK ports, so every client on the machine can use them:

```sh
./build/bindings/tedium-jack &
# ports: tedium:cv1..cv6, trig1..trig4, gate1..gate2, buttons (MIDI)
```

**Pure Data** — one object replaces all four legacy externals:

```
[tedium~]
  inlets:  2 signal, driving the gate outputs
  outlets: 6 or 8 signal CV (volts), 4 signal triggers, 1 control for buttons
  messages: smooth <hz>, cal <path>, verbose <0|1>, print
```

**Csound**:

```
acv1, acv2, acv3, acv4, acv5, acv6  tedcv        ; a-rate CV in volts
at1, at2, at3, at4                  tedtrig      ; a-rate trigger gates
                                    tedgate a1, a2
kbtn                                tedbutton 1
```

Configured by environment: `TEDIUM_BOARD`, `TEDIUM_SCAN_HZ`, `TEDIUM_RT_PRIO`,
`TEDIUM_CAL`, `TEDIUM_HAL`.

**ChucK** (needs a chuck source tree for headers):

```
make -C bindings chuck CK_INCLUDE=/path/to/chuck/core

TediumCV cv => dac;   0 => cv.select;
Tedium t;  <<< t.cv(0), t.button(0) >>>;
```

Note `.select()` rather than `.chan()`: `UGen.chan()` already exists with a
different return type and ChucK refuses to load a chugin that collides.

**C or C++** — link `libtedium.a` and skip the bindings entirely; see
`include/tedium/tedium.h`.

### Status

Built and run: the core, the tools, and the Pd, Csound and ChucK bindings.
The JACK binding compiles against stub headers only, since JACK was not
installed on the development machine. The Linux HAL has been parsed but not
compiled or run — it needs a Pi. Everything that touches real hardware is
therefore still unproven on hardware.

---

## the original software

`software/` holds the 2015-2019 externals, still usable and now with their
known defects fixed (an SPI descriptor leak, an `exit()` inside a Pd external,
undefined behaviour in the OSC client's channel cycling, and a demo patch whose
board-variant selection was arbitrary). `attic/` holds superseded code that is
not built.

## places to find Pd patches for Terminal Tedium
Here are a few places where you can find Pd patches that others have written for the Terminal Tedium:

- [Old Man Fury's Github repos](https://github.com/oldmanfury)
- [Wilsontr's Github repo](https://github.com/wilsontr/tt-patches)
- [j-p-higgins' Github repo](https://github.com/j-p-higgins/jh.tt)
- [North Coast Modular Collective's Github repo](https://github.com/NorthCoastModularCollective/Terminal-Tedium-Pd-Patches)
- [JMC64 Patches](https://github.com/JMC64/Terminal-Tedium-)
