#!/bin/sh
# An X window larger than the screen is maximized above the taskbar, below
# its title bar -- as the title bar's Maximize does -- not over everything:
# SG Office's editors size themselves to the screen and covered the taskbar.
# Here xev asks for 3000x3000: the bottom 40 pixels (the taskbar's strip)
# must not be its white, the row above them must be.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xev Xwayland; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xev -geometry 3000x3000 > /dev/null 2>&1 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2
px() { convert "$T/c.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
echo "      ${W}x$H: row $((H - 45)) $(px $((W / 2)) $((H - 45))), row $((H - 20)) $(px $((W / 2)) $((H - 20)))"
[ "$(px $((W / 2)) $((H - 45)))" = "255,255,255" ] && pass "the big window fills the screen down to the taskbar's strip" ||
    fail "the big window does not reach the taskbar's strip"
[ "$(px $((W / 2)) $((H - 20)))" != "255,255,255" ] && pass "the taskbar's strip (bottom 40 px) is not covered" ||
    fail "the big window covers the taskbar's strip"
exit $RC
