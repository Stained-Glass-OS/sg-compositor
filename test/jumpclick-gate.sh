#!/bin/sh
# A click right after the pointer jumps onto an X window reaches it. An
# absolute pointer (a tablet, a touch screen, Remote Desktop, a VM's) moves
# in one step; wlroots sends the window only "enter" there -- the motion to
# the same spot is left out -- and Xwayland put the click that followed on
# its root: an elevated program's OK did nothing, a Linux program's menu did
# not open. Entering, the window gets a motion too (seat.c). Here xev's
# window is jumped onto and clicked at once; xev must report the press.
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
unset DISPLAY WAYLAND_DISPLAY
wayland-scanner client-header "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
    wayland-scanner private-code "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/vp.c" &&
    cc -O2 -I"$T" -o "$T/vptr" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client) \
    || { echo "SKIP: cannot build the virtual pointer"; exit 77; }
cc -O2 -o "$T/rawmotion" "$HERE/test/rawmotion-client.c" $(pkg-config --cflags --libs x11 xi) ||
    { echo "SKIP: cannot build the raw motion client (libxi-dev)"; exit 77; }

WAYLAND_DEBUG=server WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xterm -geometry 150x50 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & $T/rawmotion & sleep 2; xev -geometry 300x200 -event button > $T/xev.log 2>&1 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2
ptr() { WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" "$W" "$H" "$@" >/dev/null 2>&1; sleep 1; }
# xev's window is centred (on top of the terminal): its middle
X=$((W / 2)); Y=$((H / 2 + 20))
# the pointer on the terminal first (another window of the same X server).
# A client listening for raw motion (as Wine does) has Xwayland take
# relative motion too, the way the click went astray.
RX=0; RY=0
for y in $(seq 40 20 "$H"); do for x in $(seq 20 20 "$W"); do
    [ "$(convert "$T/c.png" -format "%[fx:int(255*p{$x,$y}.r)],%[fx:int(255*p{$x,$y}.g)]" info: 2>/dev/null)" = "255,0" ] && { RX=$x; RY=$y; break 2; }
done; done
[ "$RX" -gt 0 ] || { fail "the terminal was not found"; exit 1; }
# one pointer device for the whole jump, as a tablet is (each vptr run is a
# device of its own, and a new device makes the seat enter afresh)
echo 0 > "$T/mark"; wc -l < "$T/log" > "$T/mark"
: > "$T/xev.log"
ptr m "$RX" "$RY" s 300 m "$X" "$Y" s 100 d s 80 u
grep -q 'ButtonPress' "$T/xev.log" && pass "a click right after the jump reaches the window" ||
    fail "the jump-click did not reach the window"
# what the compositor told Xwayland: entering xev's surface, a motion too
# before the button (wlroots leaves out a motion to the spot the enter gave;
# Xwayland then put the button on its root)
tail -n +"$(($(cat "$T/mark") + 1))" "$T/log" | grep -o 'wl_pointer[#@][0-9]*\.\(enter\|motion\|button\)' |
    sed 's/.*\.//' | tr '\n' ' ' > "$T/seq"
echo "      pointer events: $(cat "$T/seq")"
last=$(sed 's/ button.*//' "$T/seq" | awk '{ for (i = NF; i > 0; i--) if ($i == "enter") { for (j = i + 1; j <= NF; j++) if ($j == "motion") { print "motion"; exit } print "none"; exit } }')
[ "$last" = motion ] && pass "entering the window it is sent a motion before the click" ||
    fail "entering the window, no motion before the click: $(cat "$T/seq")"
# and one more after a motion inside, as before
: > "$T/xev.log"
ptr m $((X + 5)) $((Y + 5)) s 200 m $((X + 8)) $((Y + 8)) d s 50 u
grep -q 'ButtonPress' "$T/xev.log" && pass "a click after a motion inside reaches it too" || fail "the plain click did not reach the window"
exit $RC
