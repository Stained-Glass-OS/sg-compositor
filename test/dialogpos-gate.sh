#!/bin/sh
# Windows on the right display with a display LEFT of the main one (GitHub #17, #19). Two
# headless displays: B 1920x1080 at 0,0 (moved left in Display settings), the main one A
# 2560x1440 at 1920,0:
#   1. a native Wayland window opens on the display under the pointer (A), where it was
#      started from -- it went to the display under its middle at 0,0, the left one
#   2. an X dialog (WM_TRANSIENT_FOR: FileZilla's "Remember passwords?") opens over the
#      window it belongs to, on A, though the pointer is on B -- it was centred on the
#      pointer's display (and, framed by Wine, off the screen)
# Runs where a compositor may run (a QA VM), never on a person's desktop.
# Needs Xwayland and wlr-randr; the clients test/dialogpos-xclient.c, test/wlapp-client.c and
# test/vptr.c are built with cc, or taken prebuilt from SG_PREBUILT (dialogpos-xclient,
# wlapp, vptr) for a VM without a compiler.
# Mutants (build defines): SG_MUTANT_WLAPP_ORIGIN_DISPLAY, SG_MUTANT_DIALOG_POINTER_DISPLAY.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "$CP" ] && { pkill -P "$CP" 2>/dev/null; kill -9 "$CP" 2>/dev/null; }; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xwayland wlr-randr; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
if [ -n "${SG_PREBUILT:-}" ]; then
    XC=$SG_PREBUILT/dialogpos-xclient WL=$SG_PREBUILT/wlapp VP=$SG_PREBUILT/vptr
else
    XS=$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/stable/xdg-shell/xdg-shell.xml
    VPX=$HERE/test/wlr-virtual-pointer-unstable-v1.xml
    { cc -O2 -o "$T/xc" "$HERE/test/dialogpos-xclient.c" -lX11 &&
      wayland-scanner client-header "$XS" "$T/xdg-shell-client-protocol.h" && wayland-scanner private-code "$XS" "$T/xdg-shell.c" &&
      cc -O2 -I"$T" -o "$T/wl" "$HERE/test/wlapp-client.c" "$T/xdg-shell.c" $(pkg-config --cflags --libs wayland-client) &&
      wayland-scanner client-header "$VPX" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
      wayland-scanner private-code "$VPX" "$T/vp.c" &&
      cc -O2 -I"$T" -o "$T/vp" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client); } 2>/dev/null ||
        { echo "SKIP: the test clients did not build"; exit 77; }
    XC=$T/xc WL=$T/wl VP=$T/vp
fi
unset DISPLAY WAYLAND_DISPLAY
mkdir -m 700 "$T/run"; export XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME="$T/cfg"
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "while [ ! -e $T/go1 ]; do sleep 0.2; done; $WL -c 0000ff -s 400x300 -t App > $T/wl.out 2>&1 &
           while [ ! -e $T/go2 ]; do sleep 0.2; done; exec $XC 2560x1440+1920+0 > $T/x.out 2>&1" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -S "$T/priv.sock" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
randr() { WAYLAND_DISPLAY="$T/priv.sock" wlr-randr "$@"; }
# shellcheck disable=SC2046  # the output names, one word each
set -- $(randr 2>/dev/null | awk '/^[^ ]/ {print $1}')
[ $# -ge 2 ] || { echo "SKIP: not two displays: $(tail -3 "$T/log")"; exit 77; }
A=$1; B=$2
randr --output "$A" --custom-mode 2560x1440 --pos 1920,0 --output "$B" --custom-mode 1920x1080 --pos 0,0 ||
    { echo "SKIP: the headless displays take no custom mode"; exit 77; }
sleep 1
ptr() { WAYLAND_DISPLAY="$T/priv.sock" "$VP" 4480 1440 "$@" >/dev/null 2>&1; sleep 1; }
ptr m 3200 700
touch "$T/go1"; sleep 3
got=$(grep '^enter' "$T/wl.out" 2>/dev/null | tail -1)
[ "$got" = "enter $A" ] && pass "a Wayland window opens on the display under the pointer, the main one ($got)" ||
    fail "a Wayland window opened with the pointer on $A (right of $B): '$got', want 'enter $A'"
ptr m 900 500
touch "$T/go2"; sleep 6
main=$(grep '^main' "$T/x.out" 2>/dev/null); dlg=$(grep '^dialog' "$T/x.out" 2>/dev/null)
set -- $dlg
if [ $# -eq 5 ]; then
    cx=$(($2 + $4 / 2)); cy=$(($3 + $5 / 2))
    [ "$cx" -ge 1920 ] && [ "$cx" -lt 4480 ] && [ $((cx - 3200)) -le 40 ] && [ $((3200 - cx)) -le 40 ] && [ "$cy" -lt 1440 ] &&
        pass "an X dialog opens over its window on the main display, the pointer on the left one ($dlg; $main)" ||
        fail "an X dialog: '$dlg' (centre $cx,$cy), its window '$main' -- want centred over it at about 3200"
else
    fail "an X dialog: no place ('$dlg', '$main'): $(tail -3 "$T/log")"
fi
exit $RC
