#!/bin/sh
# An X11 window of a Linux program keeps the size it asks for, centred
# (view.c, xwayland.c): a terminal (Linux Terminal (Administrator), xterm)
# opened maximized, and -- once placed as asked -- at 1x1, because a
# managed window's ConfigureRequest (xterm maps small, then asks for its real
# size) was never answered. And a click on it reaches it: X picks the window
# a click goes to by its own idea of positions and stacking, which still had
# it at 0,0 under the full-screen Wine desktop -- sdl-freerdp's certificate
# dialog could not be accepted. Wine's windows are override-redirect and are
# not affected.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xterm xev Xwayland wayland-scanner; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
wayland-scanner client-header "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
    wayland-scanner private-code "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/vp.c" &&
    cc -O2 -I"$T" -o "$T/vptr" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client) \
    || { echo "SKIP: cannot build the virtual pointer"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xterm -geometry 80x24 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & sleep 2; stdbuf -oL xev -geometry 200x100+0+0 -event button > $T/xev.log 2>&1 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
px() { convert "$T/c.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2
CX=$((W / 2)); CY=$((H / 2))
# the terminal (red) is centred, larger than the xev window (white, 200x100)
# that sits on top of it: a point beside xev but inside the terminal
[ "$(px $((CX + 130)) $CY)" = "255,0,0" ] && pass "the terminal is shown, centred, at the size it asked for (not 1x1)" \
    || fail "beside the centre: $(px $((CX + 130)) $CY)"
[ "$(px 5 5)" != "255,0,0" ] && [ "$(px 5 5)" != "255,255,255" ] && pass "and not maximized" || fail "a window fills the screen"
[ "$(px $CX $CY)" = "255,255,255" ] && pass "the last window shown is centred on top" || fail "centre: $(px $CX $CY)"
# the last window shown (xev, 200x100) is on top, centred: click its middle
WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" "$W" "$H" m "$CX" "$CY" d s 50 u >/dev/null 2>&1; sleep 1
grep -q ButtonPress "$T/xev.log" 2>/dev/null && pass "a click on it reaches it (X has it where it is shown, on top)" \
    || fail "the click did not reach the window"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
