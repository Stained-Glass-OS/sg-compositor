#!/bin/sh
# Session state for the session's programs (Wine's WTSRegisterSessionNotification,
# wine-sg 1704): a SESSION connection on the control socket is told the state
# at once ("OK session locked=0 remote=0 shadow=0") and then, one line each,
# "lock" / "unlock" when the user locks or unlocks, nothing for a secure
# prompt, "remote-connect" / "remote-disconnect" when Remote Desktop takes the
# session and gives it back (then "lock", as it comes back locked), and
# "shadow-start" / "shadow-end" while a remote viewer is attached.
# Mutant: SG_MUTANT_SESSION_NO_LOCK (lock.c).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
RC=0; T=$(mktemp -d); CP=""
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
# shellcheck disable=SC2317  # invoked via trap
cleanup() { [ -n "$CP" ] && kill "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 77; }
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$WAYLAND_DISPLAY > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
[ -s "$T/d" ] || { echo "FAIL  the compositor did not start"; cat "$T/log"; exit 1; }

python3 - "$T/ctl.sock" <<'EOS' || RC=1
import socket, sys, time
path = sys.argv[1]
fails = 0
def check(ok, what):
    global fails
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    if not ok: fails += 1
def ctl(cmd, fd=None):
    s = socket.socket(socket.AF_UNIX); s.connect(path)
    if fd is None: s.sendall(cmd.encode() + b"\n")
    else: socket.send_fds(s, [cmd.encode() + b"\n"], [fd])
    r = s.recv(128).decode().strip(); s.close(); return r
w = socket.socket(socket.AF_UNIX); w.connect(path); w.sendall(b"SESSION\n"); w.settimeout(2)
buf = b""
def line():
    global buf
    while b"\n" not in buf:
        try:
            d = w.recv(256)
        except socket.timeout:
            return None
        if not d: return None
        buf += d
    l, buf = buf.split(b"\n", 1)
    return l.decode()
def lines(n):
    return [line() for _ in range(n)]
check(line() == "OK session locked=0 remote=0 shadow=0", "SESSION: the state at once")
ctl("LOCK")
check(line() == "lock", "LOCK: lock")
ctl("UNLOCK")
check(line() == "unlock", "UNLOCK: unlock")
ctl("SECURE")
ctl("RELEASE")
w.settimeout(0.7)
check(line() is None, "a secure prompt: nothing")
w.settimeout(2)
r = ctl("REMOTE")
if r.startswith("OK remote"):
    check(line() == "remote-connect", "REMOTE: remote-connect")
    ctl("LOCAL")
    check(lines(2) == ["remote-disconnect", "lock"], "LOCAL: remote-disconnect, then lock (back locked)")
    ctl("UNLOCK")
    check(line() == "unlock", "unlocked again")
else:
    print("      (no remote backend: %s)" % r)
a, b = socket.socketpair()
r = ctl("SHADOW view", a.fileno())
a.close()
if r.startswith("OK shadow"):
    check(line() == "shadow-start", "SHADOW: shadow-start")
    b.close()
    check(line() == "shadow-end", "the viewer gone: shadow-end")
else:
    check(False, "SHADOW: " + r)
ctl("LOCK")
check(line() == "lock", "locked")
w2 = socket.socket(socket.AF_UNIX); w2.connect(path); w2.sendall(b"SESSION\n")
check(w2.recv(128).decode().startswith("OK session locked=1"), "a new watcher is told it is locked")
sys.exit(1 if fails else 0)
EOS
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
