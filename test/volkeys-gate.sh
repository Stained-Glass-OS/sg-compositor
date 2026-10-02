#!/bin/sh
# The volume keys (David 2026-10-02): XF86AudioRaiseVolume, LowerVolume and
# Mute run sg-settingsctl (sg-session) -- "sound step up|down --chime",
# "sound mute-toggle" -- and no window gets them. A stand-in sg-settingsctl
# records what it is asked; wtype presses the keys on the privileged socket.
# Mutant: -Dc_args=-DSG_MUTANT_NO_VOLUME_KEYS fails it.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
RC=0; T=$(mktemp -d); chmod 755 "$T"; CP=""
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
# shellcheck disable=SC2317  # invoked via trap
cleanup() { set +e; [ -n "$CP" ] && kill "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v wtype >/dev/null || { echo "SKIP: wtype missing"; exit 77; }
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
mkdir -p "$T/bin"
printf '#!/bin/sh\necho "$*" >> %s/calls\n' "$T" > "$T/bin/sg-settingsctl"
chmod 755 "$T/bin/sg-settingsctl"
: > "$T/calls"
PATH="$T/bin:$PATH" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$WAYLAND_DISPLAY > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
[ -s "$T/d" ] || { echo "FAIL  the compositor did not start"; cat "$T/log"; exit 1; }
for k in XF86AudioRaiseVolume XF86AudioLowerVolume XF86AudioMute; do
    WAYLAND_DISPLAY="$T/priv.sock" wtype -k "$k" 2>/dev/null; sleep 0.5
done
sleep 0.5
grep -qx 'sound step up --chime' "$T/calls" && pass "Volume Up: one step up, with the chime" || fail "Volume Up: $(tr '\n' '|' < "$T/calls")"
grep -qx 'sound step down --chime' "$T/calls" && pass "Volume Down: one step down, with the chime" || fail "Volume Down: $(tr '\n' '|' < "$T/calls")"
grep -qx 'sound mute-toggle' "$T/calls" && pass "Mute: toggles" || fail "Mute: $(tr '\n' '|' < "$T/calls")"
[ "$(wc -l < "$T/calls")" = 3 ] && pass "once each" || fail "calls: $(tr '\n' '|' < "$T/calls")"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
