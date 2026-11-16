#!/bin/sh
# The session's own X11 programs' windows, for Wine's taskbar (session_x11.c).
# A Linux program's window (a terminal) is a top-level beside Wine's desktop
# window, which the taskbar never saw. XWINDOWS lists it -- not a window of
# a Wine program's class (*.exe) -- and XMINIMIZE, XACTIVATE and XCLOSE act
# on it by its X window id. XKILL (Task Manager's End task on a program that
# did not close) kills a hung one's process (David 2026-10-02: CPU-X hung and
# could not be ended); mutant SG_MUTANT_XKILL_DEAF fails it. A dialog whose
# program's window is not shown (Timeshift's Setup Wizard) is listed; one over
# a shown window is not (mutant SG_MUTANT_TRANSIENT_ANY lists neither). A menu
# (POPUP_MENU) or an undecorated transient popup of a program whose window is
# framed is never listed (Steam's menus; mutant SG_MUTANT_MENU_LISTED).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xterm Xwayland python3 cc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
cc -O1 -o "$T/sg-transient" "$HERE/test/sg-transient.c" -lX11 || { echo "SKIP: cannot build sg-transient"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xterm -class FakeWine.exe -T 'A Wine window' -geometry 20x5 -e sleep 600 & sleep 1; xterm -T 'Hung Program' -geometry 20x5 -e sleep 600 & echo \$! > $T/hung; $T/sg-transient & sleep 1.5; xterm -T 'Linux Terminal' -geometry 80x24 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & echo up > $T/d; exec sleep 600" \
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
grep -q "	Lone Dialog$" "$T/list" && pass "a dialog whose program's window is not shown is listed (Timeshift's Setup Wizard)" \
    || fail "the lone dialog is not listed"
grep -q "	Main Window$" "$T/list" && ! grep -q "	Owned Dialog$" "$T/list" \
    && pass "a dialog over its program's shown window is not (the window has the button)" || fail "owned dialog: $(grep -E 'Main Window|Owned Dialog' "$T/list")"
grep -qE "	Lone (Menu|Popup)$" "$T/list" && fail "a framed program's menu or popup is listed: $(grep -E 'Lone (Menu|Popup)' "$T/list")" \
    || pass "a framed program's menu and undecorated popup are not listed (nor held)"
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
# sg-session's sg-lockctl carries them for the taskbar (SG_LOCKCTL, or ../sg-session/build)
LC="${SG_LOCKCTL:-$HERE/../sg-session/build/sg-lockctl}"
if [ -x "$LC" ]; then
    SG_LOCK_CONTROL="$T/ctl.sock" "$LC" XWINDOWS | grep -q "^$ID shown" && SG_LOCK_CONTROL="$T/ctl.sock" "$LC" XMINIMIZE "$ID" >/dev/null \
        && ctl XWINDOWS | grep -q "^$ID minimized" && SG_LOCK_CONTROL="$T/ctl.sock" "$LC" XACTIVATE "$ID" >/dev/null \
        && pass "sg-lockctl carries XWINDOWS, XMINIMIZE and XACTIVATE" || fail "sg-lockctl: $(SG_LOCK_CONTROL="$T/ctl.sock" "$LC" XWINDOWS 2>&1 | head -2)"
    SG_LOCK_CONTROL="$T/ctl.sock" "$LC" XACTIVATE "12x" >/dev/null 2>&1 && fail "sg-lockctl passed a non-number" || pass "sg-lockctl takes numbers only"
    SG_LOCK_CONTROL="$T/ctl.sock" "$LC" XWINDOWS --out "$T/xlist" && grep -q "^$ID shown" "$T/xlist" && [ "$(tail -1 "$T/xlist")" = END ] \
        && pass "sg-lockctl XWINDOWS --out writes the list to a file (Wine hands a native program no pipe)" || fail "--out: $(cat "$T/xlist" 2>&1)"
fi
[ "$(ctl "XCLOSE $ID")" = OK ] && sleep 2 || fail "XCLOSE refused"
ctl XWINDOWS | grep -q "Linux Terminal" && fail "XCLOSE left the window" || pass "XCLOSE closes it (WM_DELETE_WINDOW)"
# a hung program (stopped): asked to close, it stays; XKILL ends it
HUNG=$(cat "$T/hung" 2>/dev/null)
HID=$(ctl XWINDOWS | awk -F'\t' '$2 == "Hung Program" { split($1, f, " "); print f[1] }')
kill -STOP "$HUNG" 2>/dev/null
ctl "XCLOSE $HID" >/dev/null; sleep 2
ctl XWINDOWS | grep -q "Hung Program" && [ -d "/proc/$HUNG" ] \
    && pass "a hung program asked to close stays (it does not answer)" || fail "the hung program closed by itself: test broken"
[ "$(ctl "XKILL $HID")" = OK ] && sleep 2 || fail "XKILL refused"
# ended: gone, or a zombie its parent (exec'd into sleep) never reaps
alive() { [ -d "/proc/$1" ] && ! grep -q '^[0-9]* (.*) Z' "/proc/$1/stat" 2>/dev/null; }
alive "$HUNG" && fail "XKILL left the hung program running" || pass "XKILL ends the hung program's process"
ctl XWINDOWS | grep -q "Hung Program" && fail "its window stayed" || pass "and its window is gone"
[ "$(ctl "XKILL 99999999")" = "ERR no such window" ] && pass "XKILL of an unknown window: ERR" || fail "unknown window killed"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
