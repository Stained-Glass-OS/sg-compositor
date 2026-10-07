#!/bin/sh
# A program's native Wayland window is a window (wayland_app.c). Electron 39
# and later choose Wayland by XDG_SESSION_TYPE alone, and the Claude desktop
# app came up as an xdg_toplevel: cage maximized it over the whole screen,
# taskbar included -- no taskbar button, no Alt+Tab, not movable, and once
# the desktop came in front of it no way back (David 2026-10-06).
# Here, with a plain xdg_toplevel test client (test/wlapp-client.c):
#   - it opens centred in the work area at its own size, never over the
#     taskbar's strip (dragged down there it is cut off), larger than the
#     work area it is maximized above the taskbar; full screen it may cover all
#   - XWINDOWS lists it (an id with bit 31 set, its app id and title), and
#     XMINIMIZE / XACTIVATE / XCLOSE / XKILL act on it; XDESKTOP puts the
#     shell's desktop in front of it and XACTIVATE brings it back
#   - its own move request (a press on its title bar) moves it with the pointer
#   - a client binding xdg_wm_base version 1 maps too (the first build
#     asserted on version-5 events and took the session down)
#   - the lock screen's privileged connection keeps the full-screen window
# Mutants (#define at line 1 of wayland_app.c): SG_MUTANT_WAYLAND_UNMANAGED
# (cage's treatment), SG_MUTANT_WAYLAND_OVER_TASKBAR (no cut at the taskbar).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xterm Xwayland python3 wayland-scanner pkg-config cc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
XS=$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/stable/xdg-shell/xdg-shell.xml
[ -r "$XS" ] || { echo "SKIP: no xdg-shell.xml"; exit 77; }
wayland-scanner client-header "$XS" "$T/xdg-shell-client-protocol.h" && wayland-scanner private-code "$XS" "$T/xdg-shell.c" &&
    cc -O2 -I"$T" -o "$T/wlapp" "$HERE/test/wlapp-client.c" "$T/xdg-shell.c" $(pkg-config --cflags --libs wayland-client) &&
    wayland-scanner client-header "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
    wayland-scanner private-code "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/vp.c" &&
    cc -O2 -I"$T" -o "$T/vptr" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client) \
    || { echo "SKIP: cannot build the test clients"; exit 77; }
export XDG_CONFIG_HOME="$T/cfg"   # the default 40 px taskbar
TB=40

# the shell's desktop (an xterm standing in for explorer's "shell - Wine
# Desktop", green, full screen), then the program's Wayland window (blue)
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xterm -class explorer.exe -T 'shell - Wine Desktop' -geometry 400x200+0+0 -bg '#00ff00' -fg '#00ff00' -e sleep 600 & sleep 2; $T/wlapp -c 0000ff -s 400x300 -a com.example.Electron -t 'Claude' > $T/app.log 2>&1 & echo \$! > $T/app.pid; echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 3
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n')
out=b''
while True:
    d=s.recv(4096)
    if not d: break
    out+=d
print(out.decode().strip())"; }
shot() { sleep 1; rm -f "$T/$1.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/$1.png" >/dev/null 2>&1; }
px() { convert "$T/$1.png" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }
blue() { [ "$(px "$@")" = "0,0,255" ]; }
ptr() { WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" "$W" "$H" "$@" >/dev/null 2>&1; sleep 1; }
# the blue window's left, right, top and bottom edges through x0,y0
edges() { # SHOT X0 Y0 -> L R T B
    _l=-1 _r=-1 _t=-1 _b=-1
    for x in $(seq "$2" -2 0); do blue "$1" "$x" "$3" || { _l=$((x + 1)); break; }; done; [ $_l -lt 0 ] && _l=0
    for x in $(seq "$2" 2 "$W"); do blue "$1" "$x" "$3" || { _r=$x; break; }; done
    for y in $(seq "$3" -2 0); do blue "$1" "$2" "$y" || { _t=$((y + 1)); break; }; done; [ $_t -lt 0 ] && _t=0
    for y in $(seq "$3" 2 "$H"); do blue "$1" "$2" "$y" || { _b=$y; break; }; done
    echo "$_l $_r $_t $_b"
}

shot open
[ -s "$T/open.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(convert "$T/open.png" -format "%w %h" info:); W=$1; H=$2
blue open $((W / 2)) $(((H - TB) / 2)) && ! blue open 5 5 && ! blue open $((W - 5)) $((H - 5)) \
    && pass "the window opens at its own size in the middle of the work area, not over the screen" \
    || fail "placement: centre $(px open $((W / 2)) $(((H - TB) / 2))) corners $(px open 5 5) $(px open $((W - 5)) $((H - 5)))"
set -- $(edges open $((W / 2)) $(((H - TB) / 2))); echo "      window $1..$2 x $3..$4"
L=$1 R=$2 TOP=$3
[ $((R - L)) -ge 396 ] && [ $((R - L)) -le 404 ] && pass "at the size it asked for (400 px wide)" || fail "width $((R - L))"

ctl XWINDOWS > "$T/list"; sed 's/^/      /' "$T/list"
ID=$(awk -F'\t' '$2 == "Claude" { split($1, f, " "); print f[1] }' "$T/list")
[ -n "$ID" ] && [ "$ID" -ge 2147483648 ] 2>/dev/null && grep -q "^$ID shown focused com.example.Electron	Claude" "$T/list" \
    && pass "XWINDOWS lists it for the taskbar: an id no X window has, shown, focused, its app id and title" || fail "list: $(cat "$T/list")"
[ "$(ctl "XMINIMIZE $ID")" = OK ] && shot min || fail "XMINIMIZE refused"
! blue min $((W / 2)) $(((H - TB) / 2)) && ctl XWINDOWS | grep -q "^$ID minimized" \
    && pass "XMINIMIZE (its taskbar button) hides it, and the list says minimized" || fail "minimized: $(px min $((W / 2)) $(((H - TB) / 2)))"
[ "$(ctl "XACTIVATE $ID")" = OK ] && shot act || fail "XACTIVATE refused"
blue act $((W / 2)) $(((H - TB) / 2)) && ctl XWINDOWS | grep -q "^$ID shown focused" \
    && pass "XACTIVATE brings it back, focused" || fail "activated: $(px act $((W / 2)) $(((H - TB) / 2)))"
# Alt+Tab or a Wine window brought forward: the desktop in front (XDESKTOP);
# its button then brings it back (David: "after Alt+Tab away it can't be brought back")
[ "$(ctl XDESKTOP)" = OK ] && shot desk || fail "XDESKTOP refused"
! blue desk $((W / 2)) $(((H - TB) / 2)) && ctl XWINDOWS | grep -q "^$ID shown -" \
    && pass "the shell's desktop comes in front of it (XDESKTOP); it is still listed, not focused" || fail "desktop front: $(px desk $((W / 2)) $(((H - TB) / 2)))"
[ "$(ctl "XACTIVATE $ID")" = OK ] && shot back || fail "XACTIVATE refused"
blue back $((W / 2)) $(((H - TB) / 2)) && pass "...and XACTIVATE brings it in front again" || fail "not back: $(px back $((W / 2)) $(((H - TB) / 2)))"

# its own title bar: a press on it asks to move, and the window follows the pointer
ptr m $((L + 50)) $((TOP + 10)) d s 50 m $((L + 100)) $((TOP + 30)) s 50 m $((L + 150)) $((TOP + 50)) s 50 u
shot moved
set -- $(edges moved $((L + 200)) $((TOP + 150)))
[ $(($1 - L)) -ge 96 ] && [ $(($1 - L)) -le 104 ] && [ $(($3 - TOP)) -ge 36 ] && [ $(($3 - TOP)) -le 44 ] \
    && pass "it moves when it asks (its own title bar dragged 100,40)" || fail "move: $L,$TOP -> $1,$3 ($(grep -c pressed "$T/app.log") presses)"
ML=$1 MT=$3
# dragged down onto the taskbar: cut off there, the strip stays the desktop's
ptr m $((ML + 50)) $((MT + 10)) d s 50 m $((ML + 50)) $((MT + 150)) s 50 m $((ML + 50)) $((H - 60)) s 50 u
shot low
blue low $((ML + 200)) $((H - TB - 5)) && ! blue low $((ML + 200)) $((H - TB / 2)) \
    && pass "dragged onto the taskbar, it is cut off at the taskbar's edge" \
    || fail "over the taskbar: above $(px low $((ML + 200)) $((H - TB - 5))) strip $(px low $((ML + 200)) $((H - TB / 2)))"
[ "$(ctl "XCLOSE $ID")" = OK ] && sleep 1 && grep -q closed "$T/app.log" && pass "XCLOSE asks it to close (xdg_toplevel.close)" || fail "XCLOSE: $(tail -2 "$T/app.log")"
kill "$(cat "$T/client")" "$CP" 2>/dev/null; sleep 1

# larger than the work area, and xdg_wm_base version 1: maximized above the
# taskbar; one that asks for full screen covers it all
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv2.sock" -C "$T/ctl2.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; $T/wlapp -v 1 -c 0000ff -s 3000x2000 -t Big > $T/big.log 2>&1 & echo up > $T/d2; exec sleep 600" \
    >"$T/log2" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d2" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 3
rm -f "$T/big.png"; WAYLAND_DISPLAY="$T/priv2.sock" grim "$T/big.png" >/dev/null 2>&1
grep -q "^version 1" "$T/big.log" && kill -0 "$CP" 2>/dev/null && pass "a client of xdg_wm_base version 1 maps, the compositor up" || fail "version 1: $(cat "$T/big.log") $(tail -3 "$T/log2")"
blue big 2 2 && blue big $((W - 3)) $((H - TB - 3)) && ! blue big $((W / 2)) $((H - TB / 2)) && grep -q "configure $W $((H - TB)) maximized" "$T/big.log" \
    && pass "larger than the work area: maximized there, above the taskbar" || fail "big: $(px big 2 2) $(px big $((W - 3)) $((H - TB - 3))) strip $(px big $((W / 2)) $((H - TB / 2))) $(grep configure "$T/big.log" | tail -1)"
# End task (Task Manager, a program that did not close): its process killed
BID=$(python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl2.sock'); s.sendall(b'XWINDOWS\n'); print(s.recv(4096).decode())" | awk -F'\t' '$2 == "Big" { split($1, f, " "); print f[1] }')
BP=$(pgrep -f "$T/wlapp -v 1")
r=$(python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl2.sock'); s.sendall(b'XKILL $BID\n'); print(s.recv(4096).decode().strip())")
sleep 1
[ "$r" = OK ] && [ -n "$BP" ] && ! kill -0 "$BP" 2>/dev/null && pass "XKILL ends the program's process" || fail "XKILL: '$r' pid $BP"
kill "$(cat "$T/client")" "$CP" 2>/dev/null; sleep 1

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv3.sock" -C "$T/ctl3.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; $T/wlapp -f -c 0000ff -t Video > $T/fs.log 2>&1 & echo up > $T/d3; exec sleep 600" \
    >"$T/log3" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d3" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 3
rm -f "$T/fs.png"; WAYLAND_DISPLAY="$T/priv3.sock" grim "$T/fs.png" >/dev/null 2>&1
blue fs 2 2 && blue fs $((W / 2)) $((H - 3)) && pass "full screen (it asked): the whole screen, taskbar included" || fail "fullscreen: $(px fs 2 2) $(px fs $((W / 2)) $((H - 3))) $(grep configure "$T/fs.log" | tail -1)"
kill "$(cat "$T/client")" "$CP" 2>/dev/null; sleep 1

# the lock screen's privileged connection keeps cage's window: the whole screen
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv4.sock" -C "$T/ctl4.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; echo up > $T/d4; exec sleep 600" >"$T/log4" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d4" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 2
WAYLAND_DISPLAY="$T/priv4.sock" "$T/wlapp" -c 0000ff -s 300x200 -t Lock > "$T/lock.log" 2>&1 &
LP=$!
sleep 2
rm -f "$T/lock.png"; WAYLAND_DISPLAY="$T/priv4.sock" grim "$T/lock.png" >/dev/null 2>&1
blue lock 2 2 && blue lock $((W / 2)) $((H - 3)) && [ -z "$(python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl4.sock'); s.sendall(b'XWINDOWS\n'); print(s.recv(4096).decode().replace('END','').strip())")" ] \
    && pass "a privileged client (the lock screen) still gets the whole screen, and is not on the taskbar" || fail "privileged: $(px lock 2 2) $(px lock $((W / 2)) $((H - 3)))"
kill "$LP" 2>/dev/null
echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
