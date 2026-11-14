#!/bin/sh
# The scrolling direction (Settings > Mouse, sg-settingsctl input): the
# compositor reads $XDG_CONFIG_HOME/stained-glass/input.conf at start, natural
# scrolling off for mice and touchpads unless chosen (David 2026-11-13:
# "natural scrolling should not be default"), and again within a second of it
# changing, to apply to every mouse and touchpad. Headless has no libinput
# devices: this checks what the compositor reads (its log), not a device.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "${KEEP:-}" ] && cp "$T/log" /var/tmp/inputconf.log; [ -n "$CP" ] && { pkill -P "$CP" 2>/dev/null; kill "$CP" 2>/dev/null; wait "$CP" 2>/dev/null; }; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY
mkdir -p "$T/cfg/stained-glass"
XDG_CONFIG_HOME="$T/cfg" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- sh -c "exec sleep 120" >"$T/log" 2>&1 &
CP=$!
sleep 2
kill -0 "$CP" 2>/dev/null || { fail "the compositor did not start: $(tail -3 "$T/log")"; exit 1; }
printf 'natural_mouse=0\nnatural_touchpad=1\n' > "$T/cfg/stained-glass/input.conf"
i=0; while ! grep -q 'input.conf: natural scrolling mouse=0 touchpad=1' "$T/log" && [ $i -lt 30 ]; do sleep 0.2; i=$((i + 1)); done
grep -q 'input.conf: natural scrolling mouse=0 touchpad=1' "$T/log" && pass "input.conf read within a second of being written (touchpad natural, mouse not)" \
    || fail "input.conf not read: $(grep input.conf "$T/log" | tail -2)"
sleep 1.1   # (a new modification time)
printf 'natural_mouse=0\nnatural_touchpad=0\n' > "$T/cfg/stained-glass/input.conf"
i=0; while ! grep -q 'input.conf: natural scrolling mouse=0 touchpad=0' "$T/log" && [ $i -lt 30 ]; do sleep 0.2; i=$((i + 1)); done
grep -q 'input.conf: natural scrolling mouse=0 touchpad=0' "$T/log" && pass "and read again when changed (both off)" \
    || fail "the change not read: $(grep input.conf "$T/log" | tail -2)"
exit $RC
