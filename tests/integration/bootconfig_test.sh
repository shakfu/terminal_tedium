#!/bin/sh
#
# Tests for the one part of install.sh that can brick a Pi: the marked block
# it appends to /boot/firmware/config.txt.
#
# The properties that matter:
#   1. appending never disturbs the user's existing settings
#   2. re-running leaves exactly one block, not two
#   3. --uninstall restores the file byte for byte
#   4. the block starts with [all]
#
# (4) is not cosmetic. config.txt uses conditional filter sections such as
# [pi4], [cm5] and [EDID=...] which apply to every line after them until the
# next filter. A block appended to a file whose last section is [cm5] would
# be scoped to a CM5 only: the settings would read correctly but never take
# effect. This test exists because that bug was written and caught here.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
INSTALL="$ROOT/install.sh"

MARK_BEGIN="# >>> terminal tedium >>>"
MARK_END="# <<< terminal tedium <<<"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
ok()  { printf '  ok    %s\n' "$1"; }
bad() { printf '  FAIL  %s\n' "$1"; fail=$((fail + 1)); }

printf '\ninstall.sh boot-config block\n'
printf -- '----------------------------\n'

# A config.txt that ends inside a conditional filter section, which is the
# case the naive append gets wrong.
cat > "$TMP/cfg.txt" <<'EOF'
# user's real config
dtparam=audio=on
dtoverlay=vc4-kms-v3d
max_framebuffers=2
arm_boost=1
[cm5]
dtoverlay=dwc2,dr_mode=host
EOF
cp "$TMP/cfg.txt" "$TMP/cfg.orig"

# Extract the block install.sh would write, so this test tracks the script
# rather than a copy of it that can drift.
BLOCK=$(sed -n "/^    block=\"\$MARK_BEGIN\$/,/^\$MARK_END\"\$/p" "$INSTALL" \
        | sed -e '1s/.*/'"$MARK_BEGIN"'/' -e '$s/.*/'"$MARK_END"'/' \
        | sed -e 's/\$OVERLAY/rpi-proto/')

if [ -z "$BLOCK" ]; then
    bad "could not extract the block from install.sh (did it get restructured?)"
    printf '\n%d failure(s)\n\n' "$fail"
    exit 1
fi

# 4. [all] must be present, before any dtparam/dtoverlay line.
if printf '%s\n' "$BLOCK" | grep -q '^\[all\]$'; then
    first_setting=$(printf '%s\n' "$BLOCK" | grep -n '^dt' | head -1 | cut -d: -f1)
    all_line=$(printf '%s\n' "$BLOCK" | grep -n '^\[all\]$' | head -1 | cut -d: -f1)
    if [ "$all_line" -lt "$first_setting" ]; then
        ok "block starts with [all] before any setting"
    else
        bad "[all] appears after a setting, so earlier settings stay filtered"
    fi
else
    bad "block has no [all]: settings would inherit whatever filter is in force"
fi

# 1. append preserves user content
printf '\n%s\n' "$BLOCK" >> "$TMP/cfg.txt"
missing=0
for want in "dtoverlay=vc4-kms-v3d" "arm_boost=1" "max_framebuffers=2" "[cm5]"; do
    grep -qF "$want" "$TMP/cfg.txt" || missing=1
done
if [ "$missing" = 0 ]; then ok "append preserves existing settings"
else                        bad "append lost user settings"; fi

# 2. idempotent: remove-then-append leaves one block
sed -i.bak "/^${MARK_BEGIN}$/,/^${MARK_END}$/d" "$TMP/cfg.txt"
printf '\n%s\n' "$BLOCK" >> "$TMP/cfg.txt"
n=$(grep -cF "$MARK_BEGIN" "$TMP/cfg.txt" || true)
if [ "$n" = 1 ]; then ok "re-running leaves exactly one block"
else                  bad "re-running left $n blocks"; fi

# 3. uninstall restores the original
sed -i.bak "/^${MARK_BEGIN}$/,/^${MARK_END}$/d" "$TMP/cfg.txt"
a=$(sed -e :a -e '/^\n*$/{$d;N;};/\n$/ba' "$TMP/cfg.txt")
b=$(sed -e :a -e '/^\n*$/{$d;N;};/\n$/ba' "$TMP/cfg.orig")
if [ "$a" = "$b" ]; then ok "uninstall restores the original file"
else                     bad "uninstall did not restore the original"; fi

printf -- '----------------------------\n'
if [ "$fail" = 0 ]; then printf '4 boot-config test(s) passed\n\n'
else                     printf '%d boot-config test(s) FAILED\n\n' "$fail"; fi
exit "$fail"
