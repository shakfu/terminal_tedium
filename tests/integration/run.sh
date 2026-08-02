#!/bin/sh
#
# Load each engine binding in its real host and check it produces the values
# the simulation backend should give. These are smoke tests: they prove the
# binding loads, instantiates, ticks and shuts down cleanly -- which is where
# binding bugs actually live (ABI mismatches, missing teardown hooks, method
# name collisions), not in arithmetic the unit tests already cover.
#
# Run from the repo root:  sh tests/integration/run.sh
#
# Engines that are not installed are skipped, not failed.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BIN="$ROOT/build/bindings"
fail=0
ran=0

say()  { printf '%s\n' "$*"; }
skip() { say "  SKIP  $1"; }
ok()   { say "  ok    $1"; ran=$((ran + 1)); }
bad()  { say "  FAIL  $1"; fail=$((fail + 1)); ran=$((ran + 1)); }

say ""
say "integration smoke tests (simulation backend)"
say "-------------------------------------------"

# ---------------------------------------------------------------- Pure Data
PD=$(command -v pd 2>/dev/null)
if [ -z "$PD" ]; then
    for c in /Applications/Pd*.app/Contents/Resources/bin/pd \
             /Applications/*/Pd*.app/Contents/Resources/bin/pd; do
        [ -x "$c" ] && PD="$c" && break
    done
fi
if [ -z "${PD:-}" ] || [ ! -x "$PD" ]; then
    skip "pd (not installed)"
elif [ ! -f "$BIN/tedium~.pd_darwin" ] && [ ! -f "$BIN/tedium~.pd_linux" ]; then
    skip "pd (binding not built: make bindings)"
else
    out=$("$PD" -nogui -path "$BIN" "$HERE/pd_smoke.pd" 2>&1)
    if printf '%s' "$out" | grep -q "CV1: 0" && \
       printf '%s' "$out" | grep -q "TRIG1: 0"; then
        ok "pd  [tedium~] loads, DSP runs, CV reads 0 V"
    else
        bad "pd"
        printf '%s\n' "$out" | sed 's/^/        /'
    fi
fi

# ------------------------------------------------------------------- Csound
if ! command -v csound >/dev/null 2>&1; then
    skip "csound (not installed)"
elif [ ! -f "$BIN/libtedium_opcodes.dylib" ] && \
     [ ! -f "$BIN/libtedium_opcodes.so" ]; then
    skip "csound (binding not built: make bindings)"
else
    out=$(TEDIUM_HAL=sim OPCODE6DIR64="$BIN" OPCODE6DIR="$BIN" \
          csound "$HERE/csound_smoke.csd" 2>&1)
    # A clean exit matters as much as the values: the opcodes need a reset
    # callback, and without one Csound segfaults on teardown.
    if printf '%s' "$out" | grep -q "cv_a=0.0000" && \
       printf '%s' "$out" | grep -q "0 errors in performance" && \
       ! printf '%s' "$out" | grep -qi "segmentation"; then
        ok "csound  opcodes load, CV reads 0 V, clean teardown"
    else
        bad "csound"
        printf '%s\n' "$out" | sed 's/^/        /'
    fi
fi

# -------------------------------------------------------------------- ChucK
if ! command -v chuck >/dev/null 2>&1; then
    skip "chuck (not installed)"
elif [ ! -f "$BIN/Tedium.chug" ]; then
    skip "chuck (binding not built: make bindings CK_INCLUDE=...)"
else
    out=$(TEDIUM_HAL=sim chuck --chugin:"$BIN/Tedium.chug" \
          "$HERE/chuck_smoke.ck" 2>&1)
    if printf '%s' "$out" | grep -q "numCV: 6" && \
       ! printf '%s' "$out" | grep -q "cannot override"; then
        ok "chuck  chugin loads, 6 CV channels, ugens tick"
    else
        bad "chuck"
        printf '%s\n' "$out" | sed 's/^/        /'
    fi
fi

# --------------------------------------------------------------------- JACK
if [ ! -x "$BIN/tedium-jack" ]; then
    skip "jack (binding not built: needs jack development headers)"
else
    out=$("$BIN/tedium-jack" -s -n tedium_test 2>&1 &
          sleep 2; kill %1 2>/dev/null)
    if printf '%s' "$out" | grep -q "ports:"; then
        ok "jack  client registers ports"
    else
        skip "jack (no jackd running)"
    fi
fi

# ------------------------------------------------------- install.sh
if sh "$HERE/bootconfig_test.sh" >/dev/null 2>&1; then
    ok "install.sh  boot-config block append/remove round-trip"
else
    bad "install.sh boot-config block"
    sh "$HERE/bootconfig_test.sh" 2>&1 | sed 's/^/        /'
fi

say ""
say "-------------------------------------------"
if [ "$fail" -eq 0 ]; then
    say "$ran integration test(s) passed"
else
    say "$fail of $ran integration test(s) FAILED"
fi
say ""
exit $fail
