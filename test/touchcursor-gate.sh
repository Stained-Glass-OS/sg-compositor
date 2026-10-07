#!/bin/sh
# The pointer hides while touch is used, as on Windows (David, 2026-10-07, on
# the Surface: "when it's getting touch input the mouse should hide, it just
# kinda sits in the center of the screen"). A finger on the screen hides it;
# the mouse, a touchpad or the pen moving shows it again -- with the image the
# program under it last asked for. Counted in captures with the pointer drawn
# (grim -c) on an empty screen: pixels that are not the background.
#
#   --mutant NAME: built with SG_MUTANT_NAME; must fail: TOUCH_KEEPS_CURSOR.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B=${SG_TOUCHCURSOR_BUILD:-$HERE/build-touchcursor}
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert Xwayland meson ninja; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
args="-Dtest-tablet=true -Dman-pages=disabled --buildtype=release"
if [ "${1:-}" = --mutant ]; then B="$B-mutant-$2"; args="$args -Dc_args=-DSG_MUTANT_$2"; fi
# shellcheck disable=SC2086  # meson's options
[ -f "$B/build.ninja" ] || meson setup "$B" "$HERE" $args >"$T/meson.log" 2>&1 || { cat "$T/meson.log"; echo "SKIP: cannot configure"; exit 77; }
ninja -C "$B" >"$T/ninja.log" 2>&1 || { cat "$T/ninja.log"; fail "the test build failed"; exit 1; }
COMP="$B/sg-compositor"
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


# --- the pointer, a touch, the pen again ---
start "true"
shot() { WAYLAND_DISPLAY="$T/priv.sock" grim -c "$T/$1.png" >/dev/null 2>&1; }
# pixels other than the most common colour (the empty screen's)
ink() { convert "$T/$1.png" -format %c histogram:info:- 2>/dev/null | sort -rn | awk 'NR > 1 { n += $1 } END { print n + 0 }'; }
pen "in 0.5 0.5" "move 0.5 0.5" out
shot a; A=$(ink a)
[ "${A:-0}" -gt 20 ] && pass "the pointer is drawn ($A pixels)" || { fail "no pointer to begin with ($A pixels): no cursor theme?"; echo "RESULT: FAIL"; exit 1; }
pen "tdown 1 0.8 0.8" "tup 1"
shot b; B=$(ink b)
[ "${B:-1}" -eq 0 ] && pass "a touch hides it" || fail "after a touch $B pixels are not the background (the pointer still shows)"
pen "tdown 1 0.7 0.3" "tmove 1 0.72 0.32" "tup 1"
shot c; C=$(ink c)
[ "${C:-1}" -eq 0 ] && pass "and a drag keeps it hidden" || fail "after a touch drag: $C pixels"
pen "in 0.3 0.3" "move 0.31 0.3" out
shot d; D=$(ink d)
[ "${D:-0}" -gt 20 ] && pass "the pen moving shows it again ($D pixels)" || fail "after the pen moved: $D pixels (still hidden)"
stop
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
