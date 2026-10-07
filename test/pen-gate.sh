#!/bin/sh
# A pen (a Surface's through iptsd, a drawing tablet) and a touch screen.
#
#  1. The pen drives the pointer: it moves the cursor, its tip is the left
#     button and its lower barrel button the right one -- for X programs
#     (Wine's) that take only a mouse. Before 0.2.0+sg37 sg-compositor ignored
#     tablet devices and the pen did nothing.
#  2. Its pressure and tilt reach the programs (0.2.0+sg39): through
#     tablet-v2, Xwayland's "xwayland-tablet stylus" X input device carries
#     them as valuators -- what Wine (Wintab, pointer messages), GTK and Qt
#     read. Before, programs had a mouse only: every stroke at one width.
#  3. A GTK program reads the pen as a pen with its pressure, and gets touches
#     as touches (its own scrolling and pinching).
#  4. With a second screen, the pen and the touch screen stay on the built-in
#     one: before, a touch at the panel's middle landed at the seam between
#     the two screens.
#  5. Whether a pen is in range reaches X programs (0.2.0+sg40): the root
#     window's _SG_PEN_IN_RANGE, 1 or 0 -- Xwayland's pens have no proximity
#     events, so Wine (wine-sg 1150) could not tell a pen lifted away from one
#     held still, and sent no WM_POINTERLEAVE.
#  6. A tablet plugged in later (the test build's SG_TEST_TABLET_LATE and a
#     "plug" line) gives X its pen devices then: what wine-sg 1150's
#     hotplugging is tested with.
#  7. Unplugged ("unplug", 0.2.0+sg46), X has the pen no more; plugged in
#     again, it is back and draws -- wine-sg 1420's pen coming and going.
#
# A compositor built with -Dtest-tablet=true has a pen and a touch screen fed
# from a FIFO (SG_TEST_TABLET_FIFO), their events taking the same wlr_cursor
# path a real device's do. Headless, no other input device.
#
#   --mutant NAME: built with SG_MUTANT_NAME; must fail:
#     TABLET (tablets ignored), TABLET_AS_MOUSE (no tablet-v2: no pressure),
#     NO_BUILTIN_MAP (pen and touch over every screen), NO_PEN_RANGE (no
#     _SG_PEN_IN_RANGE: Wine cannot tell a pen left), UNPLUG_KEEPS_PEN (the
#     test tablet's "unplug" leaves its pen: plugged in again, it is stuck).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B=${SG_PEN_BUILD:-$HERE/build-pen}
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xev Xwayland meson ninja cc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
args="-Dtest-tablet=true -Dman-pages=disabled --buildtype=release"
if [ "${1:-}" = --mutant ]; then B="$B-mutant-$2"; args="$args -Dc_args=-DSG_MUTANT_$2"; fi
# shellcheck disable=SC2086  # meson's options
[ -f "$B/build.ninja" ] || meson setup "$B" "$HERE" $args >"$T/meson.log" 2>&1 || { cat "$T/meson.log"; echo "SKIP: cannot configure"; exit 77; }
ninja -C "$B" >"$T/ninja.log" 2>&1 || { cat "$T/ninja.log"; fail "the test build failed"; exit 1; }
COMP="$B/sg-compositor"
cc -O -o "$T/penxi2-probe" "$HERE/test/penxi2-probe.c" -lX11 -lXi || { echo "SKIP: cannot build the X probe"; exit 77; }
GTK=1
cc -O -o "$T/pengtk-probe" "$HERE/test/pengtk-probe.c" -l:libgtk-3.so.0 -l:libgdk-3.so.0 -l:libgobject-2.0.so.0 2>/dev/null || GTK=
unset DISPLAY WAYLAND_DISPLAY
mkfifo "$T/pen"

# start CHILD [ENV=VALUE...]: a compositor hosting CHILD (which writes $T/d
# when it starts); stop: end it
start() {
    child=$1; shift
    rm -f "$T/d"
    env "$@" WAYLAND_DEBUG=server SG_TEST_TABLET_FIFO="$T/pen" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
        WLR_RENDERER=pixman "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
        sh -c "$child & echo up > $T/d; exec sleep 600" >"$T/log" 2>&1 &
    CP=$!
    _w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
    sleep 4
}
stop() { kill -9 "$CP" 2>/dev/null; wait "$CP" 2>/dev/null; CP=; }
# a real pen's events each come in a frame of their own: one line per write
pen() { for l in "$@"; do printf '%s\n' "$l" > "$T/pen"; sleep 0.25; done; sleep 0.3; }
mark() { wc -l < "$1" > "$T/mark"; }
since() { tail -n +"$(( $(cat "$T/mark") + 1 ))" "$1" | tr -d '\000'; }

# --- 1. the pen as the mouse (xev: core X events) ---
start "xev -geometry 600x400 -event mouse > $T/xev.log 2>&1"
WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
# no mouse here: the seat has a pointer only if the pen is one (wl_seat
# capabilities, bit 1)
caps=$(grep -o 'wl_seat[#@][0-9]*\.capabilities([0-9]*)' "$T/log" | tail -1 | sed 's/.*(\([0-9]*\))/\1/')
[ $(( ${caps:-0} & 1 )) = 1 ] && pass "with only a pen, the seat has a pointer (capabilities $caps)" || fail "no pointer on the seat (capabilities '${caps:-none}')"
X=$T/xev.log
mark "$X"
pen "in 0.1 0.1" "move 0.5 0.5"
since "$X" | grep -q 'MotionNotify' && pass "moving the pen moves the pointer over the window" || fail "no motion reached the window"
mark "$X"
pen "move 0.5 0.5" down up
since "$X" | grep -A2 'ButtonPress' | grep -q 'button 1,' && since "$X" | grep -q 'ButtonRelease' \
    && pass "the pen's tip clicks (button 1)" || fail "the tip did not click: $(since "$X" | grep -c Button) button events"
mark "$X"
pen "button 331 1" "button 331 0"
since "$X" | grep -A2 'ButtonPress' | grep -q 'button 3,' && pass "the barrel button is the right button (button 3)" \
    || fail "the barrel button did not right-click: $(since "$X" | grep -A2 Button | tr '\n' ' ' | cut -c1-200)"
mark "$X"
pen "move 0.5 0.5" down "move 0.52 0.5" "move 0.54 0.5" up
since "$X" | grep -q 'ButtonPress' && since "$X" | grep -A2 'MotionNotify' | grep -q 'state 0x100' && since "$X" | grep -q 'ButtonRelease' \
    && pass "a stroke (down, move, up): motion with button 1 held, then the release" \
    || fail "the stroke: $(since "$X" | grep -c -E 'Press|Release') presses/releases, $(since "$X" | grep -c 'state 0x100') held motions"
pen out
stop

# --- 2. pressure and tilt reach X programs (XI2 valuators) ---
P=$T/xi2.log
start "$T/penxi2-probe 1000 600 > $P 2>&1"
grep -q 'stylus.*pressure-axis=[0-9].*tilt-axes=[0-9]*,[0-9]' "$P" \
    && pass "X has the pen as a device with pressure and tilt ($(grep -o '"xwayland-tablet stylus[^"]*"' "$P"))" \
    || fail "no pen device with pressure and tilt in X: $(grep -c device "$P") devices"
mark "$P"
pen "in 0.5 0.5" "move 0.5 0.5" "tilt 20 -10" "pressure 0.1" down "pressure 0.7" "move 0.51 0.5" "pressure 0.3" "move 0.52 0.5" up out
since "$P" > "$T/stroke"
grep -q '^press dev=xwayland-tablet stylus' "$T/stroke" && pass "the tip is a button press of the pen device" \
    || fail "no press from the pen device: $(head -c 300 "$T/stroke")"
# 0.7 of 65535 is 45874
grep '^motion dev=xwayland-tablet stylus' "$T/stroke" | grep -q 'pressure=4587[0-9]' \
    && pass "the pen's pressure reaches the program (0.7: $(grep -o 'pressure=4587[0-9]' "$T/stroke" | head -1))" \
    || fail "no pressure 0.7 from the pen: $(grep -o 'pressure=[0-9-]*' "$T/stroke" | sort -u | tr '\n' ' ')"
grep '^motion dev=xwayland-tablet stylus' "$T/stroke" | grep -q 'pressure=1966[0-9]' \
    && pass "a lighter touch is lighter (0.3: 19660)" || fail "pressure 0.3 not seen"
grep '^motion dev=xwayland-tablet stylus' "$T/stroke" | grep -q 'tiltx=20 tilty=-10' \
    && pass "the pen's tilt reaches the program (20, -10 degrees)" \
    || fail "no tilt from the pen: $(grep -o 'tiltx=[0-9-]* tilty=[0-9-]*' "$T/stroke" | sort -u | tr '\n' ' ')"
stop

# --- 3. a GTK program: the pen as a pen, touches as touches ---
if [ -n "$GTK" ]; then
    G=$T/gtk.log
    start "GDK_BACKEND=x11 GDK_SCALE=1 NO_AT_BRIDGE=1 $T/pengtk-probe 1000 600 > $G 2>&1"
    mark "$G"
    pen "in 0.5 0.5" "move 0.5 0.5" "tilt 20 -10" down "pressure 0.7" "move 0.51 0.5" "move 0.52 0.5" up out
    since "$G" | grep '^motion source=pen' | grep -q 'pressure=0.7' \
        && pass "GTK reads the pen as a pen with its pressure ($(since "$G" | grep '^motion source=pen' | grep -o 'pressure=0.7[0-9]*' | head -1))" \
        || fail "GTK has no pen pressure: $(since "$G" | cut -c1-60 | sort | uniq -c | head -5 | tr '\n' ' ')"
    mark "$G"
    pen "tdown 1 0.45 0.5" "tdown 2 0.55 0.5" "tmove 1 0.44 0.45" "tmove 2 0.56 0.45" "tup 1" "tup 2"
    n=$(since "$G" | grep -c '^touch-begin')
    [ "$n" -ge 2 ] && pass "two fingers reach GTK as two touches ($n touch-begin)" \
        || fail "GTK got $n touch begins for two fingers: $(since "$G" | cut -c1-40 | sort | uniq -c | tr '\n' ' ')"
    stop
else
    echo "SKIP  GTK 3 not installed: the GTK probe"
fi

# --- 4. two screens: the pen and touch screen on the built-in one ---
start "sleep 600" WLR_HEADLESS_OUTPUTS=2 SG_INTERNAL_OUTPUT=HEADLESS-2
L=$T/log
mark "$L"
pen "in 0.5 0.5" out "tdown 7 0.5 0.5" "tup 7"
px=$(since "$L" | sed -n 's/.*sg-test: pen at \([0-9]*\) .*/\1/p' | tail -1)
tx=$(since "$L" | sed -n 's/.*sg-test: touch at \([0-9]*\) .*/\1/p' | tail -1)
# two 1280x720 screens side by side: the middle of the layout is x=1280; of
# HEADLESS-2, 640 or 1920 depending on which is placed first. The built-in
# screen's middle is either -- never the seam.
case "${px:-}" in 640|1920) pass "the pen's middle is the built-in screen's (x=$px)";; *) fail "the pen's middle is at x=${px:-none}, not the built-in screen's";; esac
case "${tx:-}" in 640|1920) pass "a touch at the middle lands at the built-in screen's (x=$tx)";; *) fail "a touch at the middle lands at x=${tx:-none}, not the built-in screen's";; esac
stop
start "sleep 600" WLR_HEADLESS_OUTPUTS=2 SG_INTERNAL_OUTPUT=HEADLESS-1
mark "$L"
pen "in 0.5 0.5" out
px1=$(since "$L" | sed -n 's/.*sg-test: pen at \([0-9]*\) .*/\1/p' | tail -1)
[ -n "$px1" ] && [ -n "${px:-}" ] && [ "$px1" != "$px" ] && pass "naming the other screen built-in moves the pen there (x=$px1)" \
    || fail "the pen ignores which screen is built in (x=${px1:-none} both times)"
stop

# --- 5. a pen in range and out of it, for X programs (_SG_PEN_IN_RANGE) ---
for t in xprop; do command -v "$t" >/dev/null || { echo "SKIP  xprop missing: the pen's range"; exit $RC; }; done
start "xprop -root >/dev/null 2>&1; echo \$DISPLAY > $T/dpy"
D=$(cat "$T/dpy" 2>/dev/null)
range() { [ -n "$D" ] && [ "$D" != ":0" ] && DISPLAY=$D xprop -root _SG_PEN_IN_RANGE 2>/dev/null | sed -n 's/.* = //p'; }
pen "in 0.5 0.5" "move 0.52 0.5"
[ "$(range)" = 1 ] && pass "a pen in range: the root window's _SG_PEN_IN_RANGE is 1" || fail "in range: _SG_PEN_IN_RANGE '$(range)'"
pen out
[ "$(range)" = 0 ] && pass "lifted out of range: 0 (Xwayland's pens have no proximity events; Wine sends WM_POINTERLEAVE)" \
    || fail "out of range: _SG_PEN_IN_RANGE '$(range)'"
stop

# --- 6. a tablet plugged in later (SG_TEST_TABLET_LATE): its X devices come then ---
rm -f "$T/go" "$T/go2" "$T/go3"
start "timeout 2 $T/penxi2-probe 400 300 > $T/before.log 2>&1; while [ ! -e $T/go ]; do sleep 0.2; done; timeout 2 $T/penxi2-probe 400 300 > $T/after.log 2>&1; while [ ! -e $T/go2 ]; do sleep 0.2; done; timeout 2 $T/penxi2-probe 400 300 > $T/gone.log 2>&1; while [ ! -e $T/go3 ]; do sleep 0.2; done; $T/penxi2-probe 400 300 > $T/back.log 2>&1" \
    SG_TEST_TABLET_LATE=1
grep -q stylus "$T/before.log" && fail "a pen device before the tablet was plugged in" || pass "no pen device before the tablet comes"
pen plug
touch "$T/go"
sleep 3
grep -q 'stylus.*pressure-axis=[0-9]' "$T/after.log" && pass "plugged in, the tablet's pen is an X device with pressure" \
    || fail "no pen device after the tablet came: $(grep -c device "$T/after.log") devices"
# --- 7. and unplugged in the middle of a stroke ("unplug", 0.2.0+sg46):
# Xwayland takes its pen devices out of use (wine-sg 1420's pen leaving is tested with it); plugged
# in again, the pen is back and draws
pen "in 0.5 0.5" "move 0.5 0.5" down "move 0.51 0.5" unplug
touch "$T/go2"
sleep 3
grep -q 'stylus.*use=3' "$T/gone.log" && fail "unplugged, the pen is still an X pointer: $(grep stylus "$T/gone.log")" \
    || pass "unplugged, the tablet's pen is no X pointer any more"
pen plug
touch "$T/go3"
sleep 3
mark "$T/back.log"
pen "in 0.5 0.5" "move 0.5 0.5" "pressure 0.7" down "move 0.51 0.5" up out
stop
grep -q 'stylus.*use=3.*pressure-axis=[0-9]' "$T/back.log" && since "$T/back.log" | grep '^motion dev=xwayland-tablet stylus' | grep -q 'pressure=4587[0-9]' \
    && since "$T/back.log" | grep -q '^press dev=xwayland-tablet stylus' \
    && pass "plugged in again, the pen is back: its tip and pressure reach the program" \
    || fail "plugged in again: $(grep stylus "$T/back.log" | head -1); $(since "$T/back.log" | cut -c1-40 | sort | uniq -c | tr '\n' ' ')"
exit $RC
