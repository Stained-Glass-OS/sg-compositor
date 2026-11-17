#!/bin/sh
# An X window full screen from its start fills the display it asked to be on (xwayland.c). SDL sets
# _NET_WM_STATE_FULLSCREEN before it maps a full-screen window (sdl-freerdp3 /f, and /multimon's
# first window); the window manager left it at its own size, centred -- only a request made after
# the window was mapped was honoured (test/onedisplay-gate.sh). A headless compositor with two
# outputs (1280x720 side by side):
#   - a window full screen from its start, asked at 100,100: it fills the first output;
#   - one asked at 1400,100: it fills the second; six more after it, each full screen (wlroots
#     wrote the state at the map from what it had read, and read it only when the surface came:
#     the program's full screen was lost one time in four or so);
#   - a request after the map (fullscreen-xclient) still fills its output.
# Mutant (#define at line 1 of xwayland.c): SG_MUTANT_X_FULLSCREEN_AT_MAP_IGNORED.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
for t in cc Xwayland; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "$CP" ] && { kill "$CP" 2>/dev/null; sleep 1; kill -9 "$CP" 2>/dev/null; }; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
cc -O2 -o "$T/fsinit" "$HERE/test/xfsinit-client.c" -lX11 && cc -O2 -o "$T/fsx" "$HERE/test/fullscreen-xclient.c" -lX11 ||
    { echo "SKIP: cannot build the test clients"; exit 77; }
mkdir -m 700 "$T/xdg"
unset DISPLAY WAYLAND_DISPLAY
XDG_CONFIG_HOME="$T/cfg" XDG_RUNTIME_DIR="$T/xdg" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- sh -c "echo \$DISPLAY > $T/disp; exec sleep 120" > "$T/log" 2>&1 &
CP=$!
i=0; while [ ! -s "$T/disp" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
DISPLAY=$(cat "$T/disp" 2>/dev/null); export DISPLAY
[ -n "$DISPLAY" ] || { echo "FAIL  no display from the compositor"; exit 1; }
sleep 1
# the first stays up while the second is made: X gives a client that comes after one that left
# the same window ID, and the window manager's last word on the old window could land on the new
"$T/fsinit" 100 8 > "$T/fs1.out" 2>&1 & F1=$!
i=0; while ! grep -q '^geometry' "$T/fs1.out" && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
g=$(grep '^geometry' "$T/fs1.out")
[ "$g" = "geometry 0 0 1280 720" ] && pass "full screen from its start, asked at 100,100: the first output ($g)" || fail "asked at 100,100: '$g' (want geometry 0 0 1280 720)"
g=$("$T/fsinit" 1400 | grep '^geometry'); kill "$F1" 2>/dev/null
[ "$g" = "geometry 1280 0 1280 720" ] && pass "full screen from its start, asked at 1400,100: the second output ($g)" || fail "asked at 1400,100: '$g' (want geometry 1280 0 1280 720)"
# wlroots writes the window's state when it is mapped from what it has read, and reads it only when
# the window's surface comes: one time in four or so the program's full screen was overwritten
n=0; for k in 1 2 3 4 5 6; do
    [ "$("$T/fsinit" 1400 0 | grep '^geometry')" = "geometry 1280 0 1280 720" ] && n=$((n + 1))
done
[ $n = 6 ] && pass "six more, one after another: all full screen" || fail "six more, one after another: $n full screen"
"$T/fsx" > "$T/fsx.out" 2>&1 & FP=$!   # it stays up after it prints
i=0; while ! grep -q '^geometry' "$T/fsx.out" && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
kill "$FP" 2>/dev/null
g=$(grep '^geometry' "$T/fsx.out"); set -- $g
[ "${4:-0}" = 1280 ] && [ "${5:-0}" = 720 ] && { [ "$2" = 0 ] || [ "$2" = 1280 ]; } && [ "$3" = 0 ] &&
    pass "a request after the map still fills its output ($g)" || fail "a request after the map: '$g'"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
