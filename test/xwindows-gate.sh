#!/bin/sh
# The session's own X11 programs' windows, for Wine's taskbar (session_x11.c).
# A Linux program's window (a terminal) is a top-level beside Wine's desktop
# window, which the taskbar never saw. XWINDOWS lists it -- not a window of
# a Wine program's class (*.exe) -- and XMINIMIZE, XACTIVATE and XCLOSE act
# on it by its X window id.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xterm Xwayland python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xterm -class FakeWine.exe -T 'A Wine window' -geometry 20x5 -e sleep 600 & sleep 1; xterm -T 'Linux Terminal' -geometry 80x24 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n')
out=b''
while True:
    d=s.recv(4096)
    if not d: break
    out+=d
print(out.decode().strip())"; }
shot() { sleep 1; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/$1.png" >/dev/null 2>&1; }
px() { convert "$T/$1.png" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }

ctl XWINDOWS > "$T/list"
sed 's/^/      /' "$T/list"
ID=$(awk -F'\t' '$2 == "Linux Terminal" { split($1, f, " "); print f[1] }' "$T/list")
[ -n "$ID" ] && grep -q "shown focused XTerm	Linux Terminal" "$T/list" \
    && pass "XWINDOWS lists the Linux program's window: shown, focused, its class and title" || fail "list: $(cat "$T/list")"
grep -q "A Wine window" "$T/list" && fail "a Wine program's window (*.exe) is listed" || pass "not a Wine program's (*.exe) window"
[ "$(tail -1 "$T/list")" = END ] && pass "the list ends in END" || fail "no END"
shot shown
set -- $(convert "$T/shown.png" -format "%w %h" info:); CX=$(($1 / 2)); CY=$(($2 / 2))
[ "$(px shown $CX $CY)" = "255,0,0" ] && pass "the terminal is on the screen" || fail "shown: $(px shown $CX $CY)"
[ "$(ctl "XMINIMIZE $ID")" = OK ] && shot minimized || fail "XMINIMIZE refused"
[ "$(px minimized $CX $CY)" != "255,0,0" ] && ctl XWINDOWS | grep -q "^$ID minimized" \
    && pass "XMINIMIZE hides it, and the list says minimized" || fail "minimized: $(px minimized $CX $CY) $(ctl XWINDOWS | head -1)"
[ "$(ctl "XACTIVATE $ID")" = OK ] && shot activated || fail "XACTIVATE refused"
[ "$(px activated $CX $CY)" = "255,0,0" ] && ctl XWINDOWS | grep -q "^$ID shown focused" \
    && pass "XACTIVATE shows it again, focused" || fail "activated: $(px activated $CX $CY) $(ctl XWINDOWS | head -1)"
[ "$(ctl "XACTIVATE 99999999")" = "ERR no such window" ] && pass "an unknown window: ERR" || fail "unknown window accepted"
[ "$(ctl "XCLOSE $ID")" = OK ] && sleep 2 || fail "XCLOSE refused"
ctl XWINDOWS | grep -q "Linux Terminal" && fail "XCLOSE left the window" || pass "XCLOSE closes it (WM_DELETE_WINDOW)"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
