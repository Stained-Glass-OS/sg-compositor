#!/bin/sh
# A program's window taken into another (wine-sg's embedding of a Linux
# program's window in its frame) leaves the keyboard with what is left: the
# compositor gave the program's window the keyboard as it mapped as a
# top-level of its own, and when it was taken into the frame -- unmapped as a
# top-level -- the keyboard stayed with a surface that was gone. Typing went
# nowhere until a click (David 2026-10-03: a Linux Terminal opened over
# PowerShell got no keys). Here: the desktop-like window, the program's window
# taken into it and given the X focus, keys from wtype must reach it.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
RC=0; T=$(mktemp -d /var/tmp/sg-embedfocus.XXXXXX); chmod 755 "$T"; CP=""
export XDG_RUNTIME_DIR="$T/run"; mkdir -m 700 "$T/run"
# shellcheck disable=SC2317  # invoked via trap
cleanup() { [ -n "$CP" ] && kill "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in cc wtype Xwayland; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
cc -O2 -o "$T/client" "$HERE/test/embedfocus-client.c" -lX11 || { echo "SKIP: cannot build the client"; exit 77; }
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -U "$(id -u)" -- \
    sh -c "'$T/client' > '$T/out'; sleep 600" >"$T/log" 2>&1 &
CP=$!
w=0; while ! grep -q '^READY' "$T/out" 2>/dev/null && [ $w -lt 120 ]; do sleep 0.5; w=$((w + 1)); done
grep -q '^READY' "$T/out" || { fail "the client did not start: $(cat "$T/out" 2>/dev/null) $(tail -3 "$T/log")"; exit 1; }
sleep 1
WAYLAND_DISPLAY="$T/priv.sock" wtype -k a 2>/dev/null; sleep 0.5
WAYLAND_DISPLAY="$T/priv.sock" wtype -k b 2>/dev/null; sleep 1
grep -q '^KEY prog ' "$T/out" && pass "keys reach the window taken into another, which has the X focus ($(grep -c '^KEY' "$T/out") keys)" \
    || fail "no key reached it: $(grep '^KEY' "$T/out" | tr '\n' ' ')"
kill -0 "$CP" 2>/dev/null && pass "the compositor is still running" || fail "the compositor exited"
echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
