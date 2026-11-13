#!/bin/sh
# Several displays: an X window is placed on one display, never across the
# gap between two (David 2026-11-13: the sign-in screen sat half on each).
# Two headless displays side by side, xev's white window:
#   1. asking for exactly the second display's area (the sign-in screen, a
#      full-screen program ask for theirs): it covers that display, only it
#   2. a small one: centred on one display (the pointer's), not on the gap
#   3. an X window asking for full screen (_NET_WM_STATE: Firefox, the Remote
#      Desktop client): the display it is on, not every display (David
#      2026-11-13); a native Wayland one too, and one maximized: one display
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
# run GEOMETRY: the compositor with two displays and xev asking for GEOMETRY; $T/c.png its picture
run() {
    rm -f "$T/d" "$T/client" "$T/c.png"
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
        "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
        sh -c "echo \$\$ > $T/client; ${2:-xev -geometry $1} > $T/out 2>&1 & echo up > $T/d; exec sleep 600" \
        >"$T/log" 2>&1 &
    CP=$!
    _w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
    sleep 4
    WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
    [ -n "${SHOTS:-}" ] && cp "$T/c.png" "$SHOTS/onedisplay-$(echo "$1" | tr -c "0-9x\n" _).png"
    kill "$(cat "$T/client")" 2>/dev/null; kill -9 "$CP" 2>/dev/null; wait "$CP" 2>/dev/null; CP=
    rm -f "$T/priv.sock" "$T/ctl.sock"
}
px() { convert "$T/c.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
white() { [ "$(px "$1" "$2")" = "255,255,255" ]; }
run 1x1; [ -s "$T/c.png" ] || { fail "no capture: $(tail -3 "$T/log")"; exit 1; }
set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2; D=$((W / 2))
[ "$W" -gt "$H" ] || { echo "SKIP: the two displays are not side by side (${W}x$H)"; exit 77; }
run "${D}x$H+$D+0"
white $((D + 20)) 20 && white $((W - 20)) $((H - 60)) && ! white $((D - 20)) 20 && ! white 20 20 \
    && pass "a window asking for the second display's area covers it, and only it" \
    || fail "asked for the second display (${D}x$H+$D+0): at $((D + 20)),20 $(px $((D + 20)) 20), $((W - 20)),$((H - 60)) $(px $((W - 20)) $((H - 60))), $((D - 20)),20 $(px $((D - 20)) 20)"
run 400x300
{ white $((D / 2)) $((H / 2)) || white $((D + D / 2)) $((H / 2)); } && ! white "$D" $((H / 2)) \
    && pass "a small window is centred on one display, not on the gap" \
    || fail "a small window: the displays' centres $(px $((D / 2)) $((H / 2))) $(px $((D + D / 2)) $((H / 2))), the gap $(px "$D" $((H / 2)))"
# the test clients: an X window asking for full screen, a native Wayland one
if command -v cc >/dev/null && cc -O2 -o "$T/fsx" "$HERE/test/fullscreen-xclient.c" -lX11 2>/dev/null; then
    run fsx "$T/fsx"
    g=$(grep '^geometry' "$T/out")
    set -- $g
    [ "${4:-0}" = "$D" ] && [ "${5:-0}" = "$H" ] && { [ "$2" = 0 ] || [ "$2" = "$D" ]; } && [ "$3" = 0 ] \
        && pass "an X window asking for full screen fills the display it is on ($g)" \
        || fail "an X window full screen: '$g' (one display is ${D}x$H)"
else fail "the X test client did not build"; fi
XS=$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/stable/xdg-shell/xdg-shell.xml
if [ -r "$XS" ] && command -v wayland-scanner >/dev/null &&
    wayland-scanner client-header "$XS" "$T/xdg-shell-client-protocol.h" && wayland-scanner private-code "$XS" "$T/xdg-shell.c" &&
    cc -O2 -I"$T" -o "$T/wlapp" "$HERE/test/wlapp-client.c" "$T/xdg-shell.c" $(pkg-config --cflags --libs wayland-client) 2>/dev/null; then
    run wlfs "$T/wlapp -f -c 0000ff -t Video"
    c=$(grep '^configure' "$T/out" | tail -1)
    echo "$c" | grep -q "^configure $D $H .*fullscreen" && pass "a Wayland window full screen: one display ($c)" \
        || fail "a Wayland window full screen: '$c' (one display is ${D}x$H)"
    run wlmax "$T/wlapp -m -c 0000ff -t Max"
    c=$(grep '^configure' "$T/out" | tail -1)
    set -- $c
    [ "${2:-0}" = "$D" ] && echo "$c" | grep -q maximized && pass "a Wayland window maximized: one display wide ($c)" \
        || fail "a Wayland window maximized: '$c' (one display is $D wide)"
else echo "      (no wayland-scanner/xdg-shell: the Wayland cases skipped)"; fi
exit $RC
