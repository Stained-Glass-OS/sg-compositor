#!/bin/sh
# Linux programs that draw their own title bars (GTK 4: zenity, GNOME apps)
# maximise and minimise themselves by asking the window manager. A window
# with no decorations asks maximised: it gets the screen less the taskbar's
# strip; asks minimised: it is minimised (XWINDOWS says so, the taskbar
# brings it back).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
RC=0; T=$(mktemp -d); CP=
cleanup() { [ -n "$CP" ] && kill "$CP" 2>/dev/null; [ -f "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
unset DISPLAY WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$T"
for t in Xwayland cc python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
cc -O2 -o "$T/wmreq-client" "$HERE/test/wmreq-client.c" -lX11 || { echo "SKIP: no libX11 headers"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; echo \"\$DISPLAY\" > $T/dpy; echo up > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 2
export DISPLAY="$(cat "$T/dpy")"
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n')
d=b''
while not d.endswith(b'END\n'):
    c=s.recv(4096)
    if not c: break
    d+=c
print(d.decode())"; }
"$T/wmreq-client" max > "$T/max.out" 2>&1 & MP=$!
sleep 5
size=$(cat "$T/max.out")
# the headless output is 1280x720: the screen less the taskbar's 40 px
[ "$size" = "1280x680" ] && pass "a window asking to be maximised fills the screen above the taskbar ($size)" || fail "maximise: '$size'"
kill "$MP" 2>/dev/null; sleep 1
"$T/wmreq-client" min > "$T/min.out" 2>&1 & NP=$!
sleep 5
ctl XWINDOWS | grep -q ' minimized .*wmreq' && pass "a window asking to be minimised is minimised" || fail "minimise: $(ctl XWINDOWS | tr '\n' '|')"
kill "$NP" 2>/dev/null
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
