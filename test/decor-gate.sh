#!/bin/sh
# A Linux program's X11 window has a title bar (decor.c): David -- "the
# Linux terminal does not seem to have a window decorator". The bar is above
# the window with its title, a border around both; Close asks the program
# to close; dragging the bar moves the window; Maximize fills the screen
# above the taskbar. Wine's windows are override-redirect and get none.
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

# a red terminal, then a white xev window (200x100, on top, centred)
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xterm -geometry 60x10 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & sleep 2; xev -geometry 200x100 > $T/xev.log 2>&1 & echo \$! > $T/xev.pid; echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
shot() { rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1; }
px() { convert "$T/c.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
ptr() { WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" "$W" "$H" "$@" >/dev/null 2>&1; sleep 1; }
shot
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2
# xev (white, on top, centred): its left and right edges on the middle row,
# and above it the bar (white too) up to the border
CY=$((H / 2 + 45)); XL=0; XR=0; BT=0
for x in $(seq 0 2 "$W"); do [ "$(px "$x" "$CY")" = "255,255,255" ] && { XL=$x; break; }; done
for x in $(seq "$XL" 2 "$W"); do [ "$(px "$x" "$CY")" != "255,255,255" ] && { XR=$x; break; }; done
for y in $(seq "$CY" -1 0); do [ "$(px $((XR - 100)) "$y")" != "255,255,255" ] && { BT=$((y + 1)); break; }; done
echo "      window $XL..$XR, bar from $BT"
border=$(px $((XR - 100)) $((BT - 1)))
[ "$XL" -gt 0 ] && [ "$border" = "227,227,227" ] && [ "$(px $((XR - 100)) $((BT + 16)))" = "255,255,255" ] \
    && [ $((CY - BT)) -gt 32 ] \
    && pass "a title bar is above the window, with Wine's windows' edge (#e3e3e3)" || fail "no bar: border $(px $((XR - 100)) $((BT - 1))) bar $(px $((XR - 100)) $((BT + 16)))"
ink=0
for x in $(seq $((XL + 12)) $((XL + 90))); do
    for y in $(seq $((BT + 10)) 2 $((BT + 22))); do
        r=$(px "$x" "$y" | cut -d, -f1); [ "${r:-255}" -lt 200 ] && ink=$((ink + 1))
    done
done
[ "$ink" -gt 10 ] && pass "its title is written on it" || fail "no title text on the bar ($ink)"

# Close (the rightmost 30 pixels of the bar, as Wine's buttons): xev is asked to close, and does
ptr m $((XR - 15)) $((BT + 16)) d s 50 u
sleep 1
shot
[ "$(px $((XR - 100)) "$CY")" = "255,0,0" ] && pass "Close closes the window (WM_DELETE_WINDOW)" || fail "Close: the window is still there"

# the terminal: find its left edge and top on a row through it
shot
row=$((H / 2)); L=0
for x in $(seq 0 4 "$W"); do [ "$(px "$x" "$row")" = "255,0,0" ] && { L=$x; break; }; done
col=$((L + 20)); TOP=0
for y in $(seq "$row" -2 0); do [ "$(px "$col" "$y")" != "255,0,0" ] && { TOP=$((y + 1)); break; }; done
[ "$L" -gt 0 ] && [ "$TOP" -gt 32 ] || fail "the terminal was not found ($L, $TOP)"
# drag its bar 100 pixels right and 40 down
ptr m "$col" $((TOP - 10)) d s 50 m $((col + 50)) $((TOP + 10)) s 50 m $((col + 100)) $((TOP + 30)) s 50 u
shot
L2=0
for x in $(seq 0 4 "$W"); do [ "$(px "$x" $((row + 40)))" = "255,0,0" ] && { L2=$x; break; }; done
[ $((L2 - L)) -ge 96 ] && [ $((L2 - L)) -le 104 ] && pass "dragging the bar moves the window" || fail "drag: left edge $L -> $L2"
# Maximize: the second button from the right on its bar
R=0
for x in $(seq "$W" -2 0); do [ "$(px "$x" $((row + 40)))" = "255,0,0" ] && { R=$x; break; }; done
ptr m $((R - 30 - 15)) $((TOP + 40 - 16)) d s 50 u
shot
[ "$(px 2 $((H / 2)))" = "255,0,0" ] && [ "$(px $((W - 3)) $((H / 2)))" = "255,0,0" ] && [ "$(px $((W / 2)) 16)" = "255,255,255" ] \
    && [ "$(px $((W / 2)) $((H - 20)))" != "255,0,0" ] \
    && pass "Maximize fills the width, its bar at the top, the taskbar's strip left free" || fail "maximize: $(px 2 $((H / 2))) $(px $((W / 2)) 16) $(px $((W / 2)) $((H - 20)))"
# Minimize: the third button from the right -- the window and its bar go (its
# taskbar button brings it back: XACTIVATE, test/xwindows-gate.sh)
ptr m $((W - 2 * 30 - 15)) 16 d s 50 u
shot
[ "$(px $((W / 2)) $((H / 2)))" != "255,0,0" ] && [ "$(px $((W / 2)) 16)" != "255,255,255" ] \
    && pass "Minimize hides the window and its bar" || fail "minimize: $(px $((W / 2)) $((H / 2))) bar $(px $((W / 2)) 16)"
# Wine's desktop window (explorer.exe's "<name> - Wine Desktop", the shell)
# is the screen: at the origin, whatever its size, and no title bar. Centred
# like a Linux program's window, it stayed off-origin after a resolution
# change. (An xterm stands in for it here.)
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv2.sock" -C "$T/ctl2.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client2; xterm -class explorer.exe -T 'shell - Wine Desktop' -geometry 40x8 -bg '#00ff00' -fg '#00ff00' -e sleep 600 & echo up > $T/d2; exec sleep 600" \
    >"$T/log2" 2>&1 &
CP2=$!
_w=0; while [ ! -s "$T/d2" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv2.sock" grim "$T/c.png" >/dev/null 2>&1
[ "$(px 20 20)" = "0,255,0" ] && [ "$(px 0 60)" = "0,255,0" ] && pass "Wine's desktop window is at the screen's origin" || fail "the shell's window is not at 0,0 ($(px 20 20), $(px 0 60))"
[ "$(px $((W / 2)) $((H / 2)))" != "0,255,0" ] && pass "at its own size (not stretched), with no title bar above" || fail "it fills the screen"
kill "$(cat "$T/client2" 2>/dev/null)" "$CP2" 2>/dev/null

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
