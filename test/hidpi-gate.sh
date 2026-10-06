#!/bin/sh
# High-resolution screens (David 2026-10-05, a Surface Pro 7 at 2736x1824:
# everything tiny). The compositor follows the display scale:
#   1. the pointer: before anyone signs in (no effects.conf: the login
#      screen, Setup) the screen's recommended scale's share of 24 px --
#      42 px at 2736x1824 (175%; the theme's nearest, 36 or 48), 24 at
#      1920x1080; with Settings' cursor= that size. Read from Xwayland's own
#      pointer (XFixes).
#   2. a window larger than the screen is maximized above the taskbar at
#      its scaled height (effects.conf taskbar=70 at 175%): the bottom 70 px
#      are not its white, the row above them is (a 40 px strip let it cover
#      the bar's top).
#   SG_COMPOSITOR_BIN=<build> sh test/hidpi-gate.sh
#   Mutants: SG_MUTANT_CURSOR_FIXED (seat.c), SG_MUTANT_TASKBAR_FIXED (decor.c).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xev Xwayland cc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
cc -O2 -o "$T/probe" "$HERE/test/cursorsize-probe.c" -lXfixes -lX11 || { fail "the probe did not build"; exit 1; }
unset DISPLAY WAYLAND_DISPLAY
mkdir -p "$T/cfg/stained-glass"

run() {   # SIZE CLIENT-COMMAND: the compositor headless at SIZE, running it
    [ -n "$CP" ] && { kill -9 "$CP" 2>/dev/null; wait "$CP" 2>/dev/null; CP=; }
    rm -f "$T/d" "$T/client"
    XDG_CONFIG_HOME="$T/cfg" SG_OUTPUT_SIZE=$1 WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
        "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
        sh -c "echo \$\$ > $T/client; echo \$DISPLAY > $T/dpy; $2 echo up > $T/d; exec sleep 600" >"$T/log" 2>&1 &
    CP=$!
    _w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w + 1)); done
    sleep 3
}
pointer() { DISPLAY=$(cat "$T/dpy") "$T/probe" 2>/dev/null; }

rm -f "$T/cfg/stained-glass/effects.conf"
run 2736x1824 ""
p=$(pointer)
# 42 px asked; the cursor theme gives its nearest size (36 or 48)
w=${p%x*}
[ "${w:-0}" -ge 36 ] 2>/dev/null && [ "$w" -le 48 ] && pass "no Settings yet (the login screen, Setup) at 2736x1824: a $p pointer (42 asked: 175% of 24)" \
    || fail "the pointer at 2736x1824 before sign-in: '$p' (want about 42)"
run 1920x1080 ""
p=$(pointer)
[ "$p" = 24x24 ] && pass "at 1920x1080: 24 px, as before" || fail "the pointer at 1920x1080: '$p' (want 24x24)"
printf 'taskbar=70\ncursor=48\n' > "$T/cfg/stained-glass/effects.conf"
run 2736x1824 "xev -geometry 3000x3000 >/dev/null 2>&1 &"
p=$(pointer)
[ "$p" = 48x48 ] && pass "Settings' cursor=48 is the pointer's size" || fail "with cursor=48: '$p'"
rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
if [ -s "$T/c.png" ]; then
    set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2
    px() { convert "$T/c.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
    [ "$(px $((W / 2)) $((H - 75)))" = "255,255,255" ] && [ "$(px $((W / 2)) $((H - 45)))" != "255,255,255" ] \
        && pass "a big window at 175% stops above the 70 px taskbar (row $((H - 75)) its, row $((H - 45)) not)" \
        || fail "the big window at 175%: row $((H - 75)) $(px $((W / 2)) $((H - 75))), row $((H - 45)) $(px $((W / 2)) $((H - 45)))"
else
    fail "no capture"
fi
exit $RC
