#!/bin/sh
# The shell's desktop in front of the session's Linux programs' windows, and
# back (session_x11.c). Wine's desktop window holds every Wine program's
# window, the Start menu and the taskbar; a Linux program's window (SG
# Office's editors) is a top-level above it. When a Wine window comes forward
# the taskbar asks XDESKTOP: the desktop is in front, with focus, the Linux
# window still shown (its button brings it back: XACTIVATE). A Linux program
# bringing its own window forward (_NET_ACTIVE_WINDOW) gets it in front too.
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
for t in Xwayland cc python3 grim convert xterm; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
cc -O2 -o "$T/wmreq-client" "$HERE/test/wmreq-client.c" -lX11 || { echo "SKIP: no libX11 headers"; exit 77; }

# the "desktop": a blue window of explorer.exe's class titled "... Wine Desktop"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; echo \"\$DISPLAY\" > $T/dpy; xterm -class explorer.exe -T 'shell - Wine Desktop' -geometry 400x120+0+0 -bg '#0000ff' -fg '#0000ff' -e sleep 600 & echo up > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 3
export DISPLAY="$(cat "$T/dpy")"
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

# the Linux program's green window, which asks to come forward after 9 s
"$T/wmreq-client" activate 9 > "$T/act.out" 2>&1 & AP=$!
sleep 3
shot linux
set -- $(convert "$T/linux.png" -format "%w %h" info:); CX=$(($1 / 2)); CY=$(($2 / 2))
[ "$(px linux $CX $CY)" = "0,255,0" ] && pass "the Linux program's window is in front of the desktop" || fail "start: $(px linux $CX $CY)"
ID=$(ctl XWINDOWS | awk -F'\t' '$2 == "wmreq" { split($1, f, " "); print f[1] }')
[ "$(ctl XDESKTOP)" = OK ] && shot desktop || fail "XDESKTOP refused"
[ "$(px desktop $CX $CY)" = "0,0,255" ] && pass "XDESKTOP: the desktop is in front" || fail "after XDESKTOP: $(px desktop $CX $CY)"
ctl XWINDOWS | grep -q "^$ID shown - " && pass "the Linux window is still shown (on the taskbar), no longer focused" \
    || fail "list after XDESKTOP: $(ctl XWINDOWS | tr '\n' '|')"
sleep 7
grep -q asked "$T/act.out" && shot asked
[ "$(px asked $CX $CY)" = "0,255,0" ] && ctl XWINDOWS | grep -q "^$ID shown focused" \
    && pass "the program asks its window forward (_NET_ACTIVE_WINDOW): in front, focused" \
    || fail "after _NET_ACTIVE_WINDOW: $(px asked $CX $CY) $(ctl XWINDOWS | tr '\n' '|')"
[ "$(ctl XDESKTOP)" = OK ] && [ "$(ctl "XACTIVATE $ID")" = OK ] && shot back
[ "$(px back $CX $CY)" = "0,255,0" ] && pass "XACTIVATE brings it in front of the desktop again" || fail "after XACTIVATE: $(px back $CX $CY)"
kill "$AP" 2>/dev/null
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
