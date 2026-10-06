#!/bin/sh
# A pen drives the pointer (a Surface's pen through iptsd, a drawing tablet):
# it moves the cursor, its tip is the left button and its lower barrel
# button the right one -- for X programs (Wine's) too, which take only a
# mouse. Before, sg-compositor ignored tablet devices and the pen did nothing.
#
# A compositor built with -Dtest-tablet=true has a pen fed from a FIFO
# (SG_TEST_TABLET_FIFO), its events taking the same wlr_cursor path a real
# pen's do. Headless, no other input device: the seat's pointer capability
# comes from the pen alone (a tablet PC without its keyboard). xev's window
# must see the motion, the tip as button 1, the barrel button as button 3.
#
#   --mutant: built with SG_MUTANT_TABLET (tablets ignored again); must fail.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B=${SG_PEN_BUILD:-$HERE/build-pen}
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xev Xwayland meson ninja; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
args="-Dtest-tablet=true -Dman-pages=disabled --buildtype=release"
if [ "${1:-}" = --mutant ]; then B="$B-mutant"; args="$args -Dc_args=-DSG_MUTANT_TABLET"; fi
# shellcheck disable=SC2086  # meson's options
[ -f "$B/build.ninja" ] || meson setup "$B" "$HERE" $args >"$T/meson.log" 2>&1 || { cat "$T/meson.log"; echo "SKIP: cannot configure"; exit 77; }
ninja -C "$B" >"$T/ninja.log" 2>&1 || { cat "$T/ninja.log"; fail "the test build failed"; exit 1; }
COMP="$B/sg-compositor"
unset DISPLAY WAYLAND_DISPLAY
mkfifo "$T/pen"

WAYLAND_DEBUG=server SG_TEST_TABLET_FIFO="$T/pen" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "xev -geometry 300x200 -event mouse > $T/xev.log 2>&1 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2
# no mouse here: the seat has a pointer only if the pen is one (wl_seat
# capabilities, bit 1)
caps=$(grep -o 'wl_seat[#@][0-9]*\.capabilities([0-9]*)' "$T/log" | tail -1 | sed 's/.*(\([0-9]*\))/\1/')
[ $(( ${caps:-0} & 1 )) = 1 ] && pass "with only a pen, the seat has a pointer (capabilities $caps)" || fail "no pointer on the seat (capabilities '${caps:-none}')"
pen() { printf '%s\n' "$@" > "$T/pen"; sleep 0.6; }
# xev's output since the last mark (it keeps writing at its own offset)
mark() { wc -l < "$T/xev.log" > "$T/mark"; }
since() { tail -n +"$(( $(cat "$T/mark") + 1 ))" "$T/xev.log" | tr -d '\000'; }
# xev's window is centred: its middle, as fractions of the screen
fx=0.5; fy=$(awk -v h="$H" 'BEGIN { printf "%.4f", (h / 2 + 20) / h }')
mark
pen "in 0.1 0.1" "move $fx $fy"
since | grep -q 'MotionNotify' && pass "moving the pen moves the pointer over the window" || fail "no motion reached the window"
mark
pen "move $fx $fy" down up
since | grep -A2 'ButtonPress' | grep -q 'button 1,' && since | grep -q 'ButtonRelease' \
    && pass "the pen's tip clicks (button 1)" || fail "the tip did not click: $(since | grep -c Button) button events"
mark
pen "button 331 1" "button 331 0"
since | grep -A2 'ButtonPress' | grep -q 'button 3,' && pass "the barrel button is the right button (button 3)" \
    || fail "the barrel button did not right-click: $(since | grep -A2 Button | tr '\n' ' ' | cut -c1-200)"
mark
pen "move $fx $fy" down "move $(awk -v x="$fx" 'BEGIN { print x + 0.02 }') $fy" up
since | grep -q 'ButtonPress' && since | grep -A2 'MotionNotify' | grep -q 'state 0x100' && since | grep -q 'ButtonRelease' \
    && pass "a stroke (down, move, up): motion with button 1 held, then the release" \
    || fail "the stroke: $(since | grep -c -E 'Press|Release') presses/releases, $(since | grep -c 'state 0x100') held motions"
[ "$W" -gt 0 ] || fail "no screen size"
exit $RC
