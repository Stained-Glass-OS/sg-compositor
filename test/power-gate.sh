#!/bin/sh
# Display power and idle: what Settings' "Turn off the screen after" needs.
#
# sg-session's sg-settingsctl runs
#     swayidle -w timeout N "wlopm --off '*'" resume "wlopm --on '*'"
# so the compositor must offer wlr-output-power-management (wlopm) and idle
# notifications (ext-idle-notify, swayidle), and honour idle inhibitors.
#
#   1. an ordinary client is offered output power management
#   2. wlopm turns the output off (and nothing can be captured from it) and on
#   3. XWayland's screen keeps its size across off/on (Wine's desktop)
#   4. input turns an off screen back on
#   5. swayidle's timeout fires when idle, its resume on input
#   6. input keeps resetting the idle timer
#   7. an idle inhibitor holds the timeout off; releasing it lets it fire
#   8. the exact swayidle + wlopm chain Settings starts, end to end
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
RC=0; T=$(mktemp -d); chmod 755 "$T"; CP=""; XP=""; SP=""; IP=""
XDPY=83
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
# shellcheck disable=SC2317  # invoked via trap
cleanup() {
    set +e
    for p in $IP $SP $XP $CP; do kill "$p" 2>/dev/null; done
    rm -f "/tmp/.X${XDPY}-lock" "/tmp/.X11-unix/X${XDPY}"
    rm -rf "$T"
}
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in wayland-info wlopm swayidle wtype grim Xwayland xdpyinfo wayland-scanner cc pkg-config; do
    command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }
done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }

# The inhibitor client.
P=/usr/share/wayland-protocols/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
wayland-scanner client-header "$P" "$T/idle-inhibit-unstable-v1-client-protocol.h" &&
wayland-scanner private-code "$P" "$T/idle-inhibit-protocol.c" &&
cc -o "$T/inhibitor" -I"$T" "$HERE/test/idle-inhibitor.c" "$T/idle-inhibit-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) || { echo "SKIP: cannot build the inhibitor"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$WAYLAND_DISPLAY > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
D=$(cat "$T/d" 2>/dev/null)
[ -n "$D" ] || { echo "FAIL  the compositor did not start"; cat "$T/log"; exit 1; }

state() { WAYLAND_DISPLAY="$D" wlopm 2>/dev/null | awk 'NR==1{print $2}'; }
key() { WAYLAND_DISPLAY="$T/priv.sock" wtype -k space 2>/dev/null; }
capture() { rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1; [ -s "$T/c.png" ]; }
waitfor() { _n=0; while [ $_n -lt "$2" ]; do eval "$1" && return 0; sleep 0.2; _n=$((_n+1)); done; eval "$1"; }

# 1.
WAYLAND_DISPLAY="$D" wayland-info 2>/dev/null | grep -q zwlr_output_power_manager_v1 \
    && pass "an ordinary client is offered zwlr_output_power_manager_v1" \
    || fail "no output power management for an ordinary client"

# 2.
[ "$(state)" = on ] && pass "wlopm: the output is on" || fail "wlopm: the output is '$(state)', not on"
capture && pass "the output can be captured while on" || fail "could not capture the output while on"

rm -f "/tmp/.X${XDPY}-lock"
WAYLAND_DISPLAY="$D" Xwayland ":$XDPY" -noreset >"$T/x.log" 2>&1 &
XP=$!
waitfor 'DISPLAY=:$XDPY xdpyinfo >/dev/null 2>&1' 50
dims() { DISPLAY=":$XDPY" xdpyinfo 2>/dev/null | awk '/dimensions:/{print $2; exit}'; }
before=$(dims)

WAYLAND_DISPLAY="$D" wlopm --off '*' >/dev/null 2>&1
waitfor '[ "$(state)" = off ]' 25 && pass "wlopm --off turns the output off" || fail "after wlopm --off the output is '$(state)'"
capture && fail "the output could still be captured while off (still rendering)" \
        || pass "nothing is captured from an output that is off"
sleep 1
after=$(dims)
[ -n "$before" ] && [ "$before" = "$after" ] && pass "XWayland's screen keeps its size while off ($after)" \
    || fail "XWayland's screen changed from '$before' to '$after'"
WAYLAND_DISPLAY="$D" wlopm --on '*' >/dev/null 2>&1
waitfor '[ "$(state)" = on ]' 25 && pass "wlopm --on turns it back on" || fail "after wlopm --on the output is '$(state)'"
capture && pass "the output renders again after on" || fail "could not capture after on"
[ "$(dims)" = "$before" ] && pass "XWayland's screen keeps its size after on" || fail "XWayland's screen is '$(dims)' after on"

# 4.
WAYLAND_DISPLAY="$D" wlopm --off '*' >/dev/null 2>&1
waitfor '[ "$(state)" = off ]' 25
key
waitfor '[ "$(state)" = on ]' 25 && pass "a key press turns an off screen back on" || fail "a key press left the screen '$(state)'"

# 5.
idle() {
    rm -f "$T/idle" "$T/resumed"
    WAYLAND_DISPLAY="$D" swayidle -w timeout "$1" "touch $T/idle" resume "touch $T/resumed" >/dev/null 2>&1 &
    SP=$!
    sleep 0.5
}
stopidle() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; SP=""; }
idle 2
waitfor '[ -e "$T/idle" ]' 25 && pass "swayidle's timeout fires after 2 s idle" || fail "swayidle's timeout never fired"
key
waitfor '[ -e "$T/resumed" ]' 25 && pass "swayidle's resume runs on input" || fail "swayidle's resume never ran"
stopidle

# 6.
idle 3
i=0; while [ $i -lt 7 ]; do key; sleep 1; i=$((i+1)); done
[ -e "$T/idle" ] && fail "the timeout fired while keys were being pressed" \
                 || pass "7 s of key presses every second keep a 3 s timeout from firing"
waitfor '[ -e "$T/idle" ]' 30 && pass "and it fires once the keys stop" || fail "the timeout never fired after the keys stopped"
stopidle

# 7.
WAYLAND_DISPLAY="$D" "$T/inhibitor" >"$T/inh" 2>&1 &
IP=$!
waitfor 'grep -q inhibiting "$T/inh"' 25 || fail "the inhibitor client did not start: $(cat "$T/inh")"
key
idle 2
sleep 5
[ -e "$T/idle" ] && fail "the timeout fired while an idle inhibitor was held" \
                 || pass "an idle inhibitor holds a 2 s timeout off for 5 s"
kill "$IP" 2>/dev/null; wait "$IP" 2>/dev/null; IP=""
waitfor '[ -e "$T/idle" ]' 30 && pass "the timeout fires once the inhibitor is gone" || fail "the timeout never fired after the inhibitor went"
stopidle

# 8. Settings' chain, as sg-settingsctl starts it (with seconds for minutes).
# Start from a screen that is on, so "off" is the chain's doing.
WAYLAND_DISPLAY="$D" wlopm --on '*' >/dev/null 2>&1
waitfor '[ "$(state)" = on ]' 25 || fail "could not turn the screen on before the chain"
key
WAYLAND_DISPLAY="$D" swayidle -w timeout 2 "wlopm --off '*'" resume "wlopm --on '*'" >/dev/null 2>&1 &
SP=$!
waitfor '[ "$(state)" = off ]' 40 && pass "Settings' chain: idle turns the screen off" || fail "Settings' chain: the screen stayed '$(state)'"
key
waitfor '[ "$(state)" = on ]' 25 && pass "Settings' chain: a key turns it back on" || fail "Settings' chain: the screen stayed '$(state)' after a key"
stopidle

kill "$CP" 2>/dev/null
[ $RC -eq 0 ] && echo "RESULT: PASS" || { echo "RESULT: FAIL"; tail -20 "$T/log"; }
exit $RC
