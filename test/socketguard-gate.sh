#!/bin/sh
# A compositor never takes over the lock/control sockets of one that is still running.
#
# 2026-11-22: a test compositor started with the live session's socket paths
# (/run/stained-glass-seat/seat0/<uid>/) unlinked them and bound its own; when it
# exited, the live session's lock screen could not reach its compositor and could
# never be unlocked. listen_on() now refuses a path someone still answers on, and
# removes only a stale one. Mutant SG_MUTANT_STEAL_SOCKET (the old unconditional
# unlink) must fail.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
RC=0; T=$(mktemp -d); chmod 755 "$T"; A=""; B=""
export XDG_RUNTIME_DIR="$T/run"; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
unset WAYLAND_DISPLAY DISPLAY
# shellcheck disable=SC2317  # invoked via trap
stop() { pkill -P "$1" 2>/dev/null; kill "$1" 2>/dev/null; _k=0; while kill -0 "$1" 2>/dev/null && [ $_k -lt 25 ]; do sleep 0.2; _k=$((_k+1)); done; kill -9 "$1" 2>/dev/null; }
cleanup() { [ -n "$A" ] && stop "$A"; [ -n "$B" ] && stop "$B"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 77; }
status() {   # the reply to STATUS on the control socket, or "dead"
    python3 - "$T/ctl.sock" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(3)
try:
    s.connect(sys.argv[1]); s.sendall(b"STATUS\n"); print(s.recv(200).decode().strip() or "empty")
except Exception:
    print("dead")
PY
}
start() {   # name: a headless compositor on the shared socket paths
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
        "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- sh -c "echo up > $T/$1.up; exec sleep 600" >"$T/$1.log" 2>&1 &
}
start a; A=$!
_w=0; while [ ! -s "$T/a.up" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
s=$(status); case "$s" in dead|empty) fail "the first compositor's control socket does not answer: $s"; exit 1;; esac
pass "the first compositor answers on its control socket ($s)"

start b; B=$!
_w=0; while kill -0 "$B" 2>/dev/null && [ ! -s "$T/b.up" ] && [ $_w -lt 25 ]; do sleep 0.2; _w=$((_w+1)); done
if kill -0 "$B" 2>/dev/null && [ -s "$T/b.up" ]; then
    fail "a second compositor started on the same socket paths"
    stop "$B"
else
    grep -q 'in use by a running compositor' "$T/b.log" && pass "a second compositor on the same paths refuses to start, and says why" \
        || pass "a second compositor on the same paths does not start"
fi
B=""
s=$(status); case "$s" in dead|empty) fail "after the second start the first compositor is unreachable: $s";;
    *) pass "the first compositor still answers on its control socket afterwards ($s)";; esac

# a stale socket (nobody listening) is still taken over: the next session can start
stop "$A"; A=""
[ -S "$T/ctl.sock" ] || python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])" "$T/ctl.sock"
start c; A=$!
_w=0; while [ ! -s "$T/c.up" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
s=$(status); case "$s" in dead|empty) fail "a compositor could not take over a stale socket: $s";;
    *) pass "a stale socket (nobody listening) is taken over by the next compositor";; esac

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
