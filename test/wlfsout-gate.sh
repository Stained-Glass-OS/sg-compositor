#!/bin/sh
# A native Wayland window full screen on the display it names (wayland_app.c): xdg_toplevel's
# set_fullscreen with an output, as sdl-freerdp3 /multimon asks for one window on each display. Both
# were put on the display the window was on, one over the other, the second display left empty. A
# headless compositor with two outputs (1280x720 side by side), two windows asking at once, one blue
# on HEADLESS-1, one green on HEADLESS-2: each display is filled, corner to corner, by one of them.
# Mutant (#define at line 1 of wayland_app.c): SG_MUTANT_WAYLAND_FULLSCREEN_OUTPUT_IGNORED.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
for t in grim convert Xwayland wayland-scanner pkg-config cc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
XS=$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/stable/xdg-shell/xdg-shell.xml
[ -r "$XS" ] || { echo "SKIP: no xdg-shell.xml"; exit 77; }
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "$CP" ] && { kill "$CP" 2>/dev/null; sleep 1; kill -9 "$CP" 2>/dev/null; }; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
wayland-scanner client-header "$XS" "$T/xdg-shell-client-protocol.h" && wayland-scanner private-code "$XS" "$T/xdg-shell.c" &&
    cc -O2 -I"$T" -o "$T/wlapp" "$HERE/test/wlapp-client.c" "$T/xdg-shell.c" $(pkg-config --cflags --libs wayland-client) ||
    { echo "SKIP: cannot build the test client"; exit 77; }
mkdir -m 700 "$T/xdg"
unset DISPLAY WAYLAND_DISPLAY
XDG_CONFIG_HOME="$T/cfg" XDG_RUNTIME_DIR="$T/xdg" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "$T/wlapp -f -o HEADLESS-1 -c 0000ff -t one > $T/one.log 2>&1 & $T/wlapp -f -o HEADLESS-2 -c 00ff00 -t two > $T/two.log 2>&1 & echo up > $T/d; exec sleep 120" \
    > "$T/log" 2>&1 &
CP=$!
i=0; while [ ! -s "$T/d" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
sleep 4
WAYLAND_DISPLAY="$T/priv.sock" grim "$T/s.png" >/dev/null 2>&1 || { echo "FAIL  no screenshot"; exit 1; }
px() { convert "$T/s.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
half() { # X0: the colour filling 1280x720 from X0 (its four corners), or "mixed: ..."
    _a=$(px $(($1 + 2)) 2) _b=$(px $(($1 + 1277)) 2) _c=$(px $(($1 + 2)) 717) _d=$(px $(($1 + 1277)) 717)
    [ "$_a" = "$_b" ] && [ "$_a" = "$_c" ] && [ "$_a" = "$_d" ] && echo "$_a" || echo "mixed: $_a $_b $_c $_d"
}
L=$(half 0); R=$(half 1280)
case "$L/$R" in
    0,0,255/0,255,0|0,255,0/0,0,255) pass "each display filled by the window that asked for it (left $L, right $R)" ;;
    *) fail "the displays: left $L, right $R (want one blue 0,0,255 and one green 0,255,0); $(grep configure "$T/one.log" | tail -1) / $(grep configure "$T/two.log" | tail -1)" ;;
esac
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
