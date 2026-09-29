#!/bin/sh
# A secure prompt dims the desktop rather than blanking it (lock.c set_dimmed).
# The prompt (elevation consent, ADR 0012) takes a few seconds to start; the
# screen went black for all of that time (QA: "the window goes black for 10
# sec or so then it pops up"). Now the session's windows stay to be seen,
# darkened, until the prompt covers them -- still out of reach of the
# keyboard and pointer (secure-gate.sh). RELEASE lifts the dimming; a LOCK
# still hides every window.
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
    sh -c "echo \$\$ > $T/client; xterm -geometry 80x24 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n'); print(s.recv(64).decode().strip())"; }
shot() { sleep 1; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/$1.png" >/dev/null 2>&1; }
px() { convert "$T/$1.png" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }

shot open
[ -s "$T/open.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(convert "$T/open.png" -format "%w %h" info:); CX=$(($1 / 2)); CY=$(($2 / 2))
[ "$(px open $CX $CY)" = "255,0,0" ] && pass "the session's window (red), centred" || fail "window: $(px open $CX $CY)"
back=$(px open 5 5)
[ "$(ctl SECURE)" = "OK secure" ] && shot secure || fail "SECURE refused"
r=$(px secure $CX $CY | cut -d, -f1)
[ "$(px secure $CX $CY | cut -d, -f2-)" = "0,0" ] && [ "$r" -gt 40 ] && [ "$r" -lt 150 ] \
    && pass "secure prompt: the window is still shown, dimmed ($(px secure $CX $CY)), not black" || fail "secure: $(px secure $CX $CY)"
[ "$(px secure 5 5)" != "$back" ] && [ "$(px secure 5 5)" != "0,0,0" ] && pass "and the backdrop beside it, dimmed too ($(px secure 5 5))" || fail "backdrop under the prompt: $(px secure 5 5) (was $back)"
[ "$(ctl RELEASE)" = "OK unlocked" ] && shot released || fail "RELEASE refused"
[ "$(px released $CX $CY)" = "255,0,0" ] && [ "$(px released 5 5)" = "$back" ] && pass "RELEASE: undimmed" || fail "released: $(px released $CX $CY) $(px released 5 5)"
[ "$(ctl LOCK)" = "OK locked" ] && shot locked || fail "LOCK refused"
[ "$(px locked $CX $CY)" = "$back" ] && pass "a lock still hides the window: the backdrop, undimmed" || fail "locked: $(px locked $CX $CY) (backdrop $back)"
[ "$(ctl SECURE)" = "ERR locked" ] && pass "(no secure prompt over a lock)" || fail "SECURE over a lock"
ctl UNLOCK >/dev/null

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
