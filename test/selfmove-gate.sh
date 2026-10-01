#!/bin/sh
# A Linux program that draws its own title bar is moved by dragging it: it
# asks the window manager (_NET_WM_MOVERESIZE, as Qt's startSystemMove does
# for SG Office's editors, and GTK 4 for its programs) while the button is
# held, and the compositor moves the window with the pointer until the button
# comes up -- and it stays where it was put when the program then resizes
# itself. A virtual pointer drags the window's middle by (150, 100).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert Xwayland wayland-scanner cc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$T"
wayland-scanner client-header "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
    wayland-scanner private-code "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/vp.c" &&
    cc -O2 -I"$T" -o "$T/vptr" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client) \
    || { echo "SKIP: cannot build the virtual pointer"; exit 77; }
cc -O2 -o "$T/selfmove" "$HERE/test/selfmove-client.c" -lX11 || { echo "SKIP: no libX11 headers"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; $T/selfmove > $T/move.log 2>&1 & echo up > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2
X=$((W / 2)); Y=$((H / 2))
set -- $(grep '^at ' "$T/move.log" | tail -1); X0=${2:-?}; Y0=${3:-?}
# press in the window's middle, drag by (150, 100) in steps, release
args="m $X $Y s 300 m $((X + 1)) $Y s 100 d s 200"
i=1; while [ $i -le 10 ]; do args="$args m $((X + 15 * i)) $((Y + 10 * i)) s 40"; i=$((i + 1)); done
WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" "$W" "$H" $args s 200 u >/dev/null 2>&1
sleep 2
set -- $(grep '^at ' "$T/move.log" | tail -1); X1=${2:-?}; Y1=${3:-?}
echo "      $(tr '\n' '|' < "$T/move.log")"
grep -q "asked to move" "$T/move.log" && pass "the window asked to be moved (_NET_WM_MOVERESIZE) on the press" \
    || fail "the press did not reach the window"
[ "$X1" != "?" ] && [ "$X0" != "?" ] && [ $((X1 - X0)) -ge 140 ] && [ $((X1 - X0)) -le 160 ] && \
    [ $((Y1 - Y0)) -ge 90 ] && [ $((Y1 - Y0)) -le 110 ] \
    && pass "dragged, it moved with the pointer, and stayed there when it resized itself: ($X0, $Y0) -> ($X1, $Y1)" \
    || fail "not where it was dragged: ($X0, $Y0) -> ($X1, $Y1)"
grep -q resized "$T/move.log" && pass "the program resized itself after the drag" || fail "no resize after the drag"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
