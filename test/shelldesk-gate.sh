#!/bin/sh
# The session's desktop over every display (GitHub #12). Wine keeps its
# desktop window ("shell - Wine Desktop", explorer.exe) full screen; it is
# every display, as Windows' desktop is, not the one under its middle. Two
# displays of different sizes, a 2560x1440 main one and a 1920x1080 one:
#   1. beside each other: the desktop covers both (4480x1440 at 0,0) -- it
#      was cut to the main display, the other showed nothing
#   2. the 1080p one moved to the left of the main one (Display settings'
#      arrangement): at once still both, from the new origin -- the desktop
#      kept the main display's size from 0,0, drew the left display's part on
#      the main one and stopped there
# Runs where a compositor may run (a QA VM), never on a person's desktop.
# Needs Xwayland, wlr-randr and the client test/shelldesk-xclient.c
# (SG_SHELLDESK_CLIENT, else built with cc).
# Mutant: SG_MUTANT_SHELL_DESKTOP_ONE_DISPLAY (build define).
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
CLIENT="${SG_SHELLDESK_CLIENT:-}"
if [ -z "$CLIENT" ]; then
    command -v cc >/dev/null && cc -O2 -o "$T/client" "$HERE/test/shelldesk-xclient.c" -lX11 2>/dev/null ||
        { echo "SKIP: the X test client did not build"; exit 77; }
    CLIENT=$T/client
fi
unset DISPLAY WAYLAND_DISPLAY
# a runtime directory of its own: never a session's
mkdir -m 700 "$T/run"; export XDG_RUNTIME_DIR="$T/run"
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "while [ ! -e $T/go ]; do sleep 0.2; done; exec $CLIENT > $T/out 2>&1" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -S "$T/priv.sock" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
randr() { WAYLAND_DISPLAY="$T/priv.sock" wlr-randr "$@"; }
# shellcheck disable=SC2046  # the output names, one word each
set -- $(randr 2>/dev/null | awk '/^[^ ]/ {print $1}')
[ $# -ge 2 ] || { echo "SKIP: not two displays: $(tail -3 "$T/log")"; exit 77; }
A=$1; B=$2
randr --output "$A" --custom-mode 2560x1440 --pos 0,0 --output "$B" --custom-mode 1920x1080 --pos 2560,0 ||
    { echo "SKIP: the headless displays take no custom mode"; exit 77; }
touch "$T/go"
sleep 4
got=$(grep '^geometry' "$T/out" 2>/dev/null | tail -1)
[ "$got" = "geometry 0 0 4480 1440" ] && pass "beside each other: the desktop covers both displays ($got)" ||
    fail "beside each other: the desktop is '$got', want 0 0 4480 1440 (main $A 2560x1440 at 0,0, $B 1920x1080 at 2560,0)"
randr --output "$B" --pos 0,0 --output "$A" --pos 1920,0
sleep 3
got=$(grep '^geometry' "$T/out" 2>/dev/null | tail -1)
[ "$got" = "geometry 0 0 4480 1440" ] && pass "the 1080p display moved left of the main one: the desktop covers both at once ($got)" ||
    fail "moved left: the desktop is '$got', want 0 0 4480 1440 ($B at 0,0, $A at 1920,0)"
randr --output "$A" --pos 0,0 --output "$B" --pos 0,1440
sleep 3
got=$(grep '^geometry' "$T/out" 2>/dev/null | tail -1)
[ "$got" = "geometry 0 0 2560 2520" ] && pass "moved below the main one: the desktop follows ($got)" ||
    fail "moved below: the desktop is '$got', want 0 0 2560 2520"
exit $RC
