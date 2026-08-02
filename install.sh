#!/usr/bin/env bash
#
# Terminal Tedium installer for current Raspberry Pi OS (Bookworm and later).
#
# Differences from the 2019 installer, which should not be run on a modern
# image:
#
#   - Writes /boot/firmware/config.txt, the real file since Bookworm.
#     /boot/config.txt is now a placeholder whose contents tell you not to
#     edit it, so the old installer silently configured nothing: SPI and I2S
#     were never enabled and the codec never appeared.
#   - Edits that file inside a marked block instead of overwriting it, so
#     your HDMI mode, KMS driver, wifi country and everything else survive.
#     Takes a timestamped backup first.
#   - Uses $SUDO_USER and its real home. The `pi' user stopped being the
#     default in Bullseye (2022) and does not exist on a fresh image.
#   - Installs a systemd unit rather than /etc/rc.local.
#   - Never deletes the repository it was run from. The old script removed
#     its own sources on the way out, so you could not rebuild or update in
#     place.
#   - Installs Pd from apt rather than fetching a 2018 tarball over plain
#     HTTP with no checksum.
#   - Does not link wiringPi, which its author deprecated in 2019 and which
#     cannot work on a Pi 5.
#   - Is idempotent and reversible: re-running changes nothing, --uninstall
#     removes what it added, and --dry-run shows the plan without touching
#     anything.
#   - Does not reboot without asking.
#
# Usage:
#     ./install.sh                 # everything, with prompts
#     ./install.sh --check         # report system state, change nothing
#     ./install.sh --dry-run       # print what would happen
#     ./install.sh --uninstall     # undo
#     ./install.sh --help

set -euo pipefail

# ------------------------------------------------------------------ #
# constants                                                           #
# ------------------------------------------------------------------ #

readonly MARK_BEGIN="# >>> terminal tedium >>>"
readonly MARK_END="# <<< terminal tedium <<<"
readonly LIMITS_FILE="/etc/security/limits.d/95-terminal-tedium.conf"
readonly UNIT_DIR="/etc/systemd/system"
readonly PREFIX_DEFAULT="/usr/local"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO

# ------------------------------------------------------------------ #
# options                                                             #
# ------------------------------------------------------------------ #

BOARD="wm8731"
OVERLAY=""
PREFIX="$PREFIX_DEFAULT"
DO_DEPS=1
DO_BOOT=1
DO_RT=1
DO_BUILD=1
DO_SERVICE=1
HAVE_JACK_BIN=0
DRY_RUN=0
ASSUME_YES=0
MODE="install"
ENGINES="pd"

usage() {
    cat <<EOF
Terminal Tedium installer

  ./install.sh [options]

Options
  --board NAME       wm8731 (default) or pcm5102a
  --overlay NAME     override the codec device-tree overlay
  --engines LIST     comma-separated: pd,csound,jack,none  (default: pd)
  --prefix DIR       install prefix (default $PREFIX_DEFAULT)

  --no-deps          skip apt packages
  --no-boot-config   skip /boot/firmware/config.txt
  --no-rt            skip realtime limits and group membership
  --no-build         skip building libtedium
  --no-service       skip the systemd unit

  --check            report system state and exit
  --dry-run          print actions without performing them
  --uninstall        remove what this script added
  -y, --yes          do not prompt
  -h, --help         this

Run as a normal user; the script calls sudo where it needs to.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --board)     BOARD="${2:?--board needs a value}"; shift 2 ;;
        --overlay)   OVERLAY="${2:?--overlay needs a value}"; shift 2 ;;
        --engines)   ENGINES="${2:?--engines needs a value}"; shift 2 ;;
        --prefix)    PREFIX="${2:?--prefix needs a value}"; shift 2 ;;
        --no-deps)      DO_DEPS=0; shift ;;
        --no-boot-config) DO_BOOT=0; shift ;;
        --no-rt)        DO_RT=0; shift ;;
        --no-build)     DO_BUILD=0; shift ;;
        --no-service)   DO_SERVICE=0; shift ;;
        --check)     MODE="check"; shift ;;
        --dry-run)   DRY_RUN=1; shift ;;
        --uninstall) MODE="uninstall"; shift ;;
        -y|--yes)    ASSUME_YES=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$BOARD" in
    wm8731|pcm5102a) ;;
    *) echo "unknown board '$BOARD' (expected wm8731 or pcm5102a)" >&2; exit 2 ;;
esac

# The wm8731 revision is a full codec on the rpi-proto overlay. The pcm5102a
# revision is DAC-only and uses the generic hifiberry-dac overlay. Override
# with --overlay if your build differs.
if [ -z "$OVERLAY" ]; then
    if [ "$BOARD" = "wm8731" ]; then OVERLAY="rpi-proto"
    else                             OVERLAY="hifiberry-dac"
    fi
fi

# ------------------------------------------------------------------ #
# output helpers                                                      #
# ------------------------------------------------------------------ #

if [ -t 1 ]; then
    C_B=$'\033[1m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_R=$'\033[31m'; C_0=$'\033[0m'
else
    C_B=""; C_G=""; C_Y=""; C_R=""; C_0=""
fi

step() { printf '\n%s==>%s %s%s%s\n' "$C_G" "$C_0" "$C_B" "$*" "$C_0"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '%s !! %s%s\n' "$C_Y" "$*" "$C_0" >&2; }
die()  { printf '%serror:%s %s\n' "$C_R" "$C_0" "$*" >&2; exit 1; }

# Run a privileged command, honouring --dry-run.
run() {
    if [ "$DRY_RUN" = 1 ]; then
        printf '    %s[dry-run]%s %s\n' "$C_Y" "$C_0" "$*"
    else
        "$@"
    fi
}

confirm() {
    [ "$ASSUME_YES" = 1 ] && return 0
    [ "$DRY_RUN" = 1 ] && return 0
    local reply
    printf '    %s [y/N] ' "$1"
    read -r reply </dev/tty || return 1
    [[ "$reply" =~ ^[Yy] ]]
}

# ------------------------------------------------------------------ #
# environment discovery                                               #
# ------------------------------------------------------------------ #

# The user to configure, which is not root even though we use sudo.
TARGET_USER="${SUDO_USER:-$(id -un)}"
[ "$TARGET_USER" = "root" ] && die "run as a normal user, not as root; the script calls sudo itself"
# getent is not everywhere (and is absent on macOS, where the build and test
# steps still work), so fall back rather than dying under `set -e'.
TARGET_HOME="$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6 || true)"
if [ -z "$TARGET_HOME" ]; then
    TARGET_HOME="${HOME:-/home/$TARGET_USER}"
fi

PI_MODEL="unknown"
if [ -r /proc/device-tree/model ]; then
    PI_MODEL="$(tr -d '\0' < /proc/device-tree/model)"
fi

# Bookworm moved the boot config. /boot/config.txt still exists as a
# placeholder that says "the file you are looking for has moved", so it must
# be checked second and never written.
BOOT_CONFIG=""
if [ -f /boot/firmware/config.txt ]; then
    BOOT_CONFIG=/boot/firmware/config.txt
elif [ -f /boot/config.txt ] && ! grep -qi "has moved" /boot/config.txt 2>/dev/null; then
    BOOT_CONFIG=/boot/config.txt
fi

OS_NAME="unknown"
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    OS_NAME="$(. /etc/os-release && echo "${PRETTY_NAME:-$NAME}")"
fi

is_pi() { [[ "$PI_MODEL" == *"Raspberry Pi"* ]]; }

# ------------------------------------------------------------------ #
# check                                                               #
# ------------------------------------------------------------------ #

check_one() {
    local label="$1" ok="$2" detail="${3:-}"
    if [ "$ok" = 1 ]; then
        printf '    %s[ ok ]%s %-28s %s\n' "$C_G" "$C_0" "$label" "$detail"
    else
        printf '    %s[    ]%s %-28s %s\n' "$C_Y" "$C_0" "$label" "$detail"
    fi
}

do_check() {
    step "system"
    info "model    $PI_MODEL"
    info "os       $OS_NAME"
    info "arch     $(uname -m)"
    info "kernel   $(uname -r)"
    info "user     $TARGET_USER ($TARGET_HOME)"

    step "configuration"
    check_one "boot config" \
        "$([ -n "$BOOT_CONFIG" ] && echo 1 || echo 0)" \
        "${BOOT_CONFIG:-not found}"
    if [ -n "$BOOT_CONFIG" ]; then
        check_one "  tedium block" \
            "$(grep -qF "$MARK_BEGIN" "$BOOT_CONFIG" && echo 1 || echo 0)"
        check_one "  spi enabled" \
            "$(grep -qE '^\s*dtparam=spi=on' "$BOOT_CONFIG" && echo 1 || echo 0)"
        check_one "  i2s enabled" \
            "$(grep -qE '^\s*dtparam=i2s=on' "$BOOT_CONFIG" && echo 1 || echo 0)"
        check_one "  codec overlay" \
            "$(grep -qE "^\s*dtoverlay=$OVERLAY" "$BOOT_CONFIG" && echo 1 || echo 0)" \
            "$OVERLAY"
    fi

    step "devices (present only after a reboot)"
    check_one "spidev" \
        "$(ls /dev/spidev0.* >/dev/null 2>&1 && echo 1 || echo 0)" \
        "$(ls /dev/spidev0.* 2>/dev/null | tr '\n' ' ')"
    check_one "gpiochip" \
        "$(ls /dev/gpiochip* >/dev/null 2>&1 && echo 1 || echo 0)" \
        "$(ls /dev/gpiochip* 2>/dev/null | tr '\n' ' ')"
    if command -v aplay >/dev/null 2>&1; then
        local card
        card="$(aplay -l 2>/dev/null | grep -c '^card' || true)"
        check_one "alsa playback card" "$([ "${card:-0}" -gt 0 ] && echo 1 || echo 0)" \
            "${card:-0} card(s)"
    fi

    step "permissions"
    local g
    for g in audio gpio spi i2c; do
        if getent group "$g" >/dev/null 2>&1; then
            check_one "group $g" \
                "$(id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$g" && echo 1 || echo 0)"
        fi
    done
    check_one "realtime limits" \
        "$([ -f "$LIMITS_FILE" ] && echo 1 || echo 0)" "$LIMITS_FILE"

    step "software"
    check_one "libtedium built" \
        "$([ -f "$REPO/build/libtedium.a" ] && echo 1 || echo 0)"
    check_one "tedium-monitor" \
        "$(command -v tedium-monitor >/dev/null 2>&1 && echo 1 || echo 0)" \
        "$(command -v tedium-monitor 2>/dev/null || true)"
    local e
    for e in pd csound chuck jackd; do
        check_one "$e" "$(command -v "$e" >/dev/null 2>&1 && echo 1 || echo 0)" \
            "$(command -v "$e" 2>/dev/null || true)"
    done
    check_one "systemd unit" \
        "$([ -f "$UNIT_DIR/tedium-jack.service" ] && echo 1 || echo 0)"

    printf '\n'
}

# ------------------------------------------------------------------ #
# steps                                                               #
# ------------------------------------------------------------------ #

install_deps() {
    step "packages"

    local pkgs=(build-essential pkg-config git alsa-utils libgpiod-dev gpiod)
    local want

    IFS=',' read -ra want <<< "$ENGINES"
    for e in "${want[@]}"; do
        case "$e" in
            pd)     pkgs+=(puredata puredata-dev) ;;
            csound) pkgs+=(csound libcsound64-dev) ;;
            jack)   pkgs+=(jackd2 libjack-jackd2-dev) ;;
            none|"") ;;
            chuck)  warn "chuck is not packaged for Debian; build it from source and pass CK_INCLUDE to 'make bindings'" ;;
            *)      warn "unknown engine '$e', ignoring" ;;
        esac
    done

    info "installing: ${pkgs[*]}"
    run sudo apt-get update -qq
    # Missing packages should not abort the whole install; report and continue.
    if ! run sudo apt-get install -y --no-install-recommends "${pkgs[@]}"; then
        warn "some packages failed to install; continuing"
        warn "re-run with --no-deps once you have sorted them out"
    fi
}

configure_boot() {
    step "boot configuration"

    [ -n "$BOOT_CONFIG" ] || die "no writable boot config found. Expected /boot/firmware/config.txt"
    info "file: $BOOT_CONFIG"

    if grep -qF "$MARK_BEGIN" "$BOOT_CONFIG"; then
        info "tedium block already present; refreshing it"
        remove_boot_block
    fi

    local backup
    backup="${BOOT_CONFIG}.tedium-backup-$(date +%Y%m%d%H%M%S)"
    info "backup: $backup"
    run sudo cp -p "$BOOT_CONFIG" "$backup"

    # Appended in a marked block so the rest of the file is untouched, and so
    # --uninstall can remove exactly what was added.
    #
    # Deliberately NOT included, unlike the 2019 config:
    #   gpu_mem=16        legacy-only; ignored under the KMS driver
    #   dtoverlay=i2s-mmap  removed from Raspberry Pi OS; ASoC handles mmap
    # The leading [all] is essential, not cosmetic. config.txt uses
    # conditional filter sections ([pi4], [cm5], [EDID=...]) that apply to
    # every line after them until the next filter. Appending to a file whose
    # last section is, say, [cm5] would silently scope these settings to a
    # CM5 only -- the settings would appear correct on inspection and simply
    # never take effect. [all] clears any filter in force.
    local block
    block="$MARK_BEGIN
# Added by terminal_tedium install.sh. Edit inside this block or remove it
# with ./install.sh --uninstall.
[all]
dtparam=audio=off
dtparam=spi=on
dtparam=i2c_arm=on
dtparam=i2s=on
dtoverlay=$OVERLAY
$MARK_END"

    if [ "$DRY_RUN" = 1 ]; then
        printf '    %s[dry-run]%s would append:\n' "$C_Y" "$C_0"
        printf '        %s\n' "$block"
    else
        printf '\n%s\n' "$block" | sudo tee -a "$BOOT_CONFIG" >/dev/null
    fi

    info "board $BOARD, overlay $OVERLAY"
    warn "takes effect after a reboot"
}

remove_boot_block() {
    [ -n "$BOOT_CONFIG" ] || return 0
    grep -qF "$MARK_BEGIN" "$BOOT_CONFIG" || return 0
    if [ "$DRY_RUN" = 1 ]; then
        printf '    %s[dry-run]%s would remove the tedium block from %s\n' \
            "$C_Y" "$C_0" "$BOOT_CONFIG"
        return 0
    fi
    sudo sed -i "/^${MARK_BEGIN}$/,/^${MARK_END}$/d" "$BOOT_CONFIG"
}

configure_rt() {
    step "realtime and permissions"

    local g
    for g in audio gpio spi i2c; do
        if getent group "$g" >/dev/null 2>&1; then
            if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$g"; then
                info "already in group $g"
            else
                info "adding $TARGET_USER to group $g"
                run sudo usermod -aG "$g" "$TARGET_USER"
            fi
        fi
    done

    # Lets the sampling thread actually get SCHED_FIFO. Without this,
    # tt_open()'s rt_priority request silently fails and you see the cost as
    # scan overruns in tedium-bench rather than as an error.
    info "realtime limits: $LIMITS_FILE"
    if [ "$DRY_RUN" = 1 ]; then
        printf '    %s[dry-run]%s would write %s\n' "$C_Y" "$C_0" "$LIMITS_FILE"
    else
        sudo tee "$LIMITS_FILE" >/dev/null <<EOF
# terminal tedium: allow the audio group to run realtime threads.
# Written by install.sh; remove with ./install.sh --uninstall.
@audio   -  rtprio     95
@audio   -  memlock    unlimited
@audio   -  nice      -19
EOF
    fi

    warn "group changes take effect at your next login"
}

build_software() {
    step "building libtedium"

    command -v make >/dev/null 2>&1 || die "make not found; run with --deps or install build-essential"

    info "make -C $REPO"
    run make -C "$REPO" clean
    run make -C "$REPO"

    info "running the test suite"
    if ! run make -C "$REPO" test; then
        die "tests failed; not installing. This is a real failure, not a formality."
    fi

    info "building engine bindings"
    # Bindings are best-effort: a missing SDK is a skip, not a failure.
    run make -C "$REPO" bindings || warn "some bindings did not build (missing SDK?)"

    info "installing to $PREFIX"
    run sudo make -C "$REPO" install PREFIX="$PREFIX"

    # `make install' covers the library and the tools, but tedium-jack is
    # built by the bindings Makefile and would otherwise never reach PREFIX
    # -- leaving the systemd unit pointing at a file that does not exist.
    if [ -x "$REPO/build/bindings/tedium-jack" ]; then
        info "installing tedium-jack to $PREFIX/bin"
        run sudo install -m 755 "$REPO/build/bindings/tedium-jack" "$PREFIX/bin/"
        HAVE_JACK_BIN=1
    else
        info "tedium-jack not built (no JACK development headers); skipping"
    fi

    # Pd looks for externals in its own extra/ directory.
    local pd_extra
    for pd_extra in /usr/lib/puredata/extra /usr/local/lib/pd/extra; do
        if [ -d "$pd_extra" ]; then
            local ext
            ext="$(ls "$REPO"/build/bindings/tedium~.pd_* 2>/dev/null | head -1 || true)"
            if [ -n "$ext" ]; then
                info "installing $(basename "$ext") to $pd_extra"
                run sudo install -m 644 "$ext" "$pd_extra/"
            fi
            break
        fi
    done
}

install_service() {
    step "systemd unit"

    if [ "${HAVE_JACK_BIN:-0}" != 1 ]; then
        info "tedium-jack was not built, so there is nothing to run; skipping"
        info "install jackd2 and libjack-jackd2-dev, then re-run with --no-deps"
        return 0
    fi

    # Installed but NOT enabled: starting hardware at boot should be an
    # explicit decision, and rc.local's habit of doing it silently is one of
    # the things this replaces.
    local unit="$UNIT_DIR/tedium-jack.service"
    info "writing $unit (not enabled)"

    if [ "$DRY_RUN" = 1 ]; then
        printf '    %s[dry-run]%s would write %s\n' "$C_Y" "$C_0" "$unit"
    else
        sudo tee "$unit" >/dev/null <<EOF
[Unit]
Description=Terminal Tedium CV/gate bridge for JACK
Documentation=file://$REPO/ARCHITECTURE.md
After=sound.target
Wants=sound.target

[Service]
Type=simple
User=$TARGET_USER
# Adjust the board and scan rate to taste; see tedium-bench for what your
# hardware actually sustains.
ExecStart=$PREFIX/bin/tedium-jack -b $BOARD -r 4000 -p 70
Restart=on-failure
RestartSec=2
# Needed for SCHED_FIFO. The limits.d file covers interactive logins;
# systemd units do not read it.
LimitRTPRIO=95
LimitMEMLOCK=infinity
Nice=-10

[Install]
WantedBy=default.target
EOF
        sudo systemctl daemon-reload
    fi

    info "enable it with: sudo systemctl enable --now tedium-jack"
}

do_uninstall() {
    step "uninstalling"

    remove_boot_block
    info "removed the boot config block (a backup of the original remains)"

    if [ -f "$LIMITS_FILE" ]; then
        info "removing $LIMITS_FILE"
        run sudo rm -f "$LIMITS_FILE"
    fi

    local unit="$UNIT_DIR/tedium-jack.service"
    if [ -f "$unit" ]; then
        info "removing $unit"
        run sudo systemctl disable --now tedium-jack 2>/dev/null || true
        run sudo rm -f "$unit"
        run sudo systemctl daemon-reload
    fi

    local f
    for f in "$PREFIX/bin/tedium-monitor" "$PREFIX/bin/tedium-cal" \
             "$PREFIX/bin/tedium-bench" "$PREFIX/bin/tedium-jack" \
             "$PREFIX/lib/libtedium.a" "$PREFIX/lib/libtedium.so"; do
        if [ -e "$f" ]; then info "removing $f"; run sudo rm -f "$f"; fi
    done
    if [ -d "$PREFIX/include/tedium" ]; then
        run sudo rm -rf "$PREFIX/include/tedium"
    fi

    # Group membership is left alone on purpose: the user may want those
    # groups for other reasons, and removing them is not our call.
    info "group membership left unchanged"
    info "the repository itself is untouched"
    printf '\n'
}

# ------------------------------------------------------------------ #
# main                                                                #
# ------------------------------------------------------------------ #

main() {
    printf '%s\n' "$C_B>>> terminal tedium <<<$C_0"

    if [ "$MODE" = "check" ]; then
        do_check
        exit 0
    fi

    if ! is_pi; then
        warn "this does not look like a Raspberry Pi (model: $PI_MODEL)"
        warn "the build and tests will work anywhere; hardware configuration will not"
        confirm "continue anyway?" || exit 1
        DO_BOOT=0
        DO_SERVICE=0
    fi

    if [ "$MODE" = "uninstall" ]; then
        confirm "remove terminal tedium configuration and installed files?" || exit 1
        do_uninstall
        exit 0
    fi

    step "plan"
    info "model     $PI_MODEL"
    info "os        $OS_NAME ($(uname -m))"
    info "user      $TARGET_USER"
    info "repo      $REPO"
    info "board     $BOARD (overlay $OVERLAY)"
    info "prefix    $PREFIX"
    printf '\n'
    if [ "$DO_DEPS" = 1 ];    then info "  * install packages ($ENGINES)"; fi
    if [ "$DO_BOOT" = 1 ];    then info "  * configure $BOOT_CONFIG"; fi
    if [ "$DO_RT" = 1 ];      then info "  * realtime limits and group membership"; fi
    if [ "$DO_BUILD" = 1 ];   then info "  * build, test and install libtedium"; fi
    if [ "$DO_SERVICE" = 1 ]; then info "  * install a systemd unit (not enabled)"; fi
    printf '\n'
    info "nothing outside these steps is modified, and the repository is never deleted."

    if [ "$DRY_RUN" = 0 ]; then
        confirm "proceed?" || exit 1
    fi

    if [ "$DO_DEPS" = 1 ];    then install_deps;    fi
    if [ "$DO_BOOT" = 1 ];    then configure_boot;  fi
    if [ "$DO_RT" = 1 ];      then configure_rt;    fi
    if [ "$DO_BUILD" = 1 ];   then build_software;  fi
    if [ "$DO_SERVICE" = 1 ]; then install_service; fi

    step "done"
    info "next:"
    info "  1. reboot, so the device-tree overlay and group changes take effect"
    info "  2. ./install.sh --check     confirm spidev, gpiochip and the codec appeared"
    info "  3. tedium-monitor           watch every input live"
    info "  4. tedium-cal               calibrate before trusting any pitch CV"
    info "  5. tedium-bench             see what scan rate your board sustains"
    printf '\n'

    if [ "$DRY_RUN" = 0 ] && [ "$DO_BOOT" = 1 ]; then
        if confirm "reboot now?"; then
            run sudo reboot
        else
            info "remember to reboot before using the hardware"
        fi
    fi
}

main "$@"
