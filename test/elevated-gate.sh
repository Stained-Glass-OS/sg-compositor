#!/bin/sh
# Elevated programs' displays (ADR 0012, bug B56), on the real compositor.
#
# The session runs as this user; the elevated program runs as another account
# (sgsystem, through sudo) on the display sg-session's sg-elevated-run gives
# it: its own Xwayland, handed to the compositor with ELEVATED. The fixture
# program (test/elevated-fixture.c) is a magenta window that logs its keys.
#
#   1. the elevated window appears in the desktop, and the user's keyboard
#      (the privileged virtual keyboard, as remote access and the gates type)
#      reaches it -- and not the session
#   2. a session program cannot drive or read it: no connection to its X
#      server, XTEST and XSendEvent reach nothing, XGetImage sees nothing
#   3. Alt+Tab leaves it for the session; ACTIVATE (the taskbar) brings it back
#   4. clipboard: elevated -> session text is offered; session -> elevated
#      only after the user's own input in the elevated window, text only
#   5. a lock hides it and keeps its keys; it comes back on unlock
#   6. the display ends with the program; only SYSTEM or root may hand one over
#
# A build that puts the elevated program on the session's display must fail
# section 2 (SG_ELEVATED_RUN=<mutant>; see CLAUDE.md). Needs sudo -n to run as
# $SG_ELEVATED_USER (default sgsystem); 77 = a prerequisite is missing.
set -u
# Never the real desktop: nothing here may inherit a DISPLAY (an adversary run
# with the caller's :0 types into whatever has the focus there).
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
SG_SESSION="${SG_SESSION:-$HERE/../sg-session}"
SG_SESSION=$(CDPATH='' cd -- "$SG_SESSION" 2>/dev/null && pwd || printf '%s' "$SG_SESSION")
RUN="${SG_ELEVATED_RUN:-$SG_SESSION/build/sg-elevated-run}"
VKBD="${SG_VKBD:-$SG_SESSION/build/sg-vkbd}"
EUSER="${SG_ELEVATED_USER:-sgsystem}"
RC=0
T=$(mktemp -d)
chmod 755 "$T"
CPID=""; EPID=""; PPID_=""; XCPID=""
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# shellcheck disable=SC2317  # invoked via trap
cleanup() {
    [ -n "$PPID_" ] && kill "$PPID_" 2>/dev/null
    [ -n "$XCPID" ] && kill "$XCPID" 2>/dev/null
    [ -n "$EPID" ] && sudo -n pkill -u "$EUSER" -f "$T/" 2>/dev/null
    [ -n "$CPID" ] && kill -9 "$CPID" 2>/dev/null
    [ -n "${SG_KEEP:-}" ] && { echo "kept $T"; return; }
    sudo -n rm -rf "$T" 2>/dev/null || rm -rf "$T"
}
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for t in Xwayland xclip grim convert cc sudo; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] && [ -x "$RUN" ] && [ -x "$VKBD" ] || { echo "SKIP: need the compositor, sg-elevated-run and sg-vkbd"; exit 77; }
id "$EUSER" >/dev/null 2>&1 && [ "$(id -u "$EUSER")" != "$(id -u)" ] || { echo "SKIP: no account $EUSER"; exit 77; }
sudo -n -u "$EUSER" true 2>/dev/null || { echo "SKIP: cannot run as $EUSER (sudo -n)"; exit 77; }
EUID_=$(id -u "$EUSER")
cc -O2 -o "$T/fx" "$HERE/test/elevated-fixture.c" -lX11 -lXtst || { echo "SKIP: cannot build the fixture"; exit 77; }
wayland-scanner client-header "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
    wayland-scanner private-code "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/vp.c" &&
    cc -O2 -I"$T" -o "$T/vptr" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client) \
    || { echo "SKIP: cannot build the virtual pointer"; exit 77; }
mkdir -m 1777 "$T/e"

as_e() { sudo -n -u "$EUSER" env "$@"; }
ctl() { python3 -c "
import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(sys.argv[1].encode()+b'\n')
d=b''
while True:
    c=s.recv(4096)
    if not c: break
    d+=c
print(d.decode().strip())" "$1"; }
ectl() { as_e python3 -c "
import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(sys.argv[1].encode()+b'\n'); print(s.recv(4096).decode().strip())" "$1"; }
type_keys() { as_e WAYLAND_DISPLAY="$T/priv.sock" "$VKBD" "$@"; }
ptr() { as_e WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" 1280 720 "$@"; }
keys() { grep -c '^KEY [a-z]$' "$T/e/target.log" 2>/dev/null; }
sent() { grep -c '^SENT ' "$T/e/target.log" 2>/dev/null; }
downs() { grep -c '^DOWN ' "$T/keypoll.log" 2>/dev/null; }
skeys() { grep -cE '^(KEY|SENT) ' "$T/session.log" 2>/dev/null; }
row() { ctl WINDOWS | grep " $WIN " ; }

cat > "$T/session.sh" <<EOS
#!/bin/sh
echo "\$DISPLAY" > "$T/userdisplay"
# What the interim design would have done (B56): let the SYSTEM account onto the
# session's display. A build that then uses it must fail section 2.
xhost +si:localuser:$EUSER >/dev/null 2>&1
"$T/fx" keypoll "$T/keypoll.log" &
# The session's own window (its desktop, as Wine's virtual desktop is): white,
# full screen, logging the keys it gets.
exec "$T/fx" target "$T/session.log" sg-session-window '#ffffff'

EOS
chmod +x "$T/session.sh"

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$EUID_" -- "$T/session.sh" >"$T/comp.log" 2>&1 &
CPID=$!
_w=0; while ! { grep -q POLLING "$T/keypoll.log" && grep -q '^WINDOW' "$T/session.log"; } 2>/dev/null && [ $_w -lt 60 ]; do sleep 0.5; _w=$((_w+1)); done
grep -q '^WINDOW' "$T/session.log" 2>/dev/null || { fail "the session never started"; exit 1; }
sleep 1
UD=$(cat "$T/userdisplay")
case "$UD" in :[0-9]*) ;; *) fail "no session display"; exit 1 ;; esac
[ "$UD" = ":0" ] && { fail "the session display is :0 -- refusing to touch it"; exit 1; }

# The elevated program: records its environment (so the gate can act as it for
# the clipboard), then runs the fixture. The broker passes the requester's
# DISPLAY; sg-elevated-run must replace it.
cat > "$T/e/elevated.sh" <<EOS
#!/bin/sh
env | grep -E '^(DISPLAY|XAUTHORITY|SG_WINSTATION)=' > "$T/e/env"
exec "$T/fx" target "$T/e/target.log"
EOS
chmod 755 "$T/e/elevated.sh"
as_e DISPLAY="$UD" SG_ELEVATED_XLOG=1 "$RUN" --control "$T/ctl.sock" --uid "$(id -u)" -- "$T/e/elevated.sh" \
    >"$T/run.log" 2>&1 &
EPID=$!
_w=0; while ! grep -q '^WINDOW ' "$T/e/target.log" 2>/dev/null && [ $_w -lt 60 ]; do sleep 0.5; _w=$((_w+1)); done
sleep 2
WIN=$(sed -n 's/^WINDOW //p' "$T/e/target.log" 2>/dev/null | head -1)
EDPY=$(sed -n 's/^DISPLAY=//p' "$T/e/env" 2>/dev/null)
EAUTH=$(sed -n 's/^XAUTHORITY=//p' "$T/e/env" 2>/dev/null)
[ -n "$WIN" ] || { fail "the elevated program never showed a window"; cat "$T/run.log"; tail -5 "$T/comp.log"; exit 1; }
echo "      elevated display $EDPY, window $WIN, session display $UD"

# --- 1. It appears, and the user's keyboard reaches it --------------------
if [ "$EDPY" != "$UD" ] && sudo -n -u "$EUSER" test -r "$EAUTH"; then pass "the elevated program has a display of its own ($EDPY, not the session's $UD)"
else fail "the elevated program runs on the session's display ($EDPY)"; fi
case "$(sed -n 's/^SG_WINSTATION=//p' "$T/e/env")" in
    'WinSta0\sg-elevated-'*) pass "and a Wine desktop of its own (SG_WINSTATION)" ;;
    *) fail "no desktop of its own: [$(sed -n 's/^SG_WINSTATION=//p' "$T/e/env")]" ;;
esac
r=$(row)
case "$r" in
    *" 200 150 300 200 shown focused "*) pass "its window is managed, shown where it asked, and focused [$r]" ;;
    *) fail "its window is not shown and focused: [$r]"; ctl WINDOWS ;;
esac
as_e WAYLAND_DISPLAY="$T/priv.sock" grim "$T/e/screen.png" 2>/dev/null
mag=$(convert "$T/e/screen.png" -crop 300x200+200+150 -fill black +opaque '#ff00ff' -format '%[fx:mean*w*h]' info: 2>/dev/null | cut -d. -f1)
if [ "${mag:-0}" -gt 10000 ]; then pass "it is on screen: the desktop shows its pixels there (privileged capture)"
else fail "its pixels are not on screen (${mag:-0} magenta)"; fi

type_keys "w"    # the first key after a keymap upload may be lost to it
sleep 1
k0=$(keys); d0=$(downs); w0=$(skeys)
type_keys "elevkeys"
sleep 2
got=$(( $(keys) - k0 )); leak=$(( $(downs) - d0 + $(skeys) - w0 ))
[ "$got" -eq 8 ] && pass "the user's keyboard reaches it ($got keys)" || fail "the user's keyboard: $got of 8 keys arrived"
[ "$leak" -eq 0 ] && pass "and the session sees none of them" || fail "the session saw $leak key-down samples"

# --- 2. A session program cannot drive or read it --------------------------
k0=$(keys); s0=$(sent); d0=$(downs)
DISPLAY="$UD" "$T/fx" adversary "$EDPY" "$WIN" > "$T/adv.txt" 2>&1
sleep 2
cat "$T/adv.txt" | sed 's/^/      adversary: /'
grep -qx 'CONNECT refused' "$T/adv.txt" && pass "a session program cannot connect to the elevated display" \
    || fail "a session program connected to the elevated display"
if cat "$EAUTH" >/dev/null 2>&1 || ls "$(dirname "$EAUTH")" >/dev/null 2>&1; then fail "a session program can read the display's cookie"
else pass "nor read its cookie ($EAUTH)"; fi
[ "$(( $(keys) - k0 ))" -eq 0 ] && pass "XTEST reached nothing in the elevated window" \
    || fail "XTEST typed $(( $(keys) - k0 )) keys into the elevated window"
[ "$(( $(sent) - s0 ))" -eq 0 ] && pass "XSendEvent reached nothing in the elevated window" \
    || fail "XSendEvent delivered $(( $(sent) - s0 )) keys to the elevated window"
[ "$(sed -n 's/^XGETIMAGE //p' "$T/adv.txt")" = 0 ] && pass "XGetImage read none of its pixels" \
    || fail "XGetImage read $(sed -n 's/^XGETIMAGE //p' "$T/adv.txt") of its pixels"
r=$(row)
case "$r" in *" focused "*) pass "and it kept the focus" ;; *) fail "it lost the focus: [$r]" ;; esac

# --- 3. Alt+Tab to the session, ACTIVATE back ------------------------------
type_keys -M alt -k Tab -m alt
sleep 1
r=$(row)
case "$r" in *" shown - "*) pass "Alt+Tab in it goes back to the session (it stays on screen)" ;; *) fail "Alt+Tab: [$r]" ;; esac
k0=$(keys); w0=$(skeys)
type_keys "zz"
sleep 1
[ "$(( $(keys) - k0 ))" -eq 0 ] && [ "$(( $(skeys) - w0 ))" -eq 2 ] && pass "keys now go to the session, not to it" \
    || fail "after Alt+Tab: elevated got $(( $(keys) - k0 )), the session's window $(( $(skeys) - w0 ))"
# Teeth for section 2: the same attack, now that the session has the focus,
# does reach the session's own window -- and still not the elevated one.
k0=$(keys); s0=$(sent); w0=$(skeys)
DISPLAY="$UD" "$T/fx" adversary "$EDPY" "$WIN" > "$T/adv2.txt" 2>&1
sleep 2
if [ "$(( $(skeys) - w0 ))" -ge 9 ]; then pass "the attack works on the session's own window (teeth: $(( $(skeys) - w0 )) keys)"
else fail "XTEST/XSendEvent reached nothing in the session either -- section 2 proves nothing"; fi
[ "$(( $(keys) - k0 + $(sent) - s0 ))" -eq 0 ] && pass "and still nothing in the elevated window" \
    || fail "the attack reached the elevated window ($(( $(keys) - k0 + $(sent) - s0 )))"
# The user copies text in the session, while it has the focus.
printf 'fromsession' | DISPLAY="$UD" xclip -selection clipboard -i & XCPID=$!
sleep 1
[ "$(ctl "ACTIVATE ${EDPY#:} $WIN")" = OK ] && pass "ACTIVATE from the session's account (the taskbar)" || fail "ACTIVATE refused"
sleep 1
owner() { as_e DISPLAY="$EDPY" XAUTHORITY="$EAUTH" "$T/fx" owner 2>/dev/null; }
[ "$(owner)" = "OWNER 0x0" ] && pass "activating it did not offer it the session's clipboard" \
    || fail "the session's clipboard was offered on ACTIVATE, without the user's input [$(owner)]"
k0=$(keys)
type_keys "back"
sleep 1
[ "$(( $(keys) - k0 ))" -eq 4 ] && pass "and it has the keyboard again" || fail "after ACTIVATE: $(( $(keys) - k0 )) of 4 keys"

# The real pointer (the privileged virtual pointer): a click on it after the
# session had the focus gives it back the focus; a drag moves it.
type_keys -M alt -k Tab -m alt
sleep 1
ptr m 640 360 d u
sleep 1
case "$(row)" in *" shown - "*) : ;; *) fail "the session did not take the focus back: [$(row)]" ;; esac
ptr m 350 250 d m 400 290 m 450 330 u
sleep 1
r=$(row)
case "$r" in
    *" 300 230 300 200 shown focused "*) pass "the pointer focuses it and drags it (moved by 100,80) [$r]" ;;
    *) fail "click and drag: [$r]" ;;
esac
ptr m 400 280 d m 300 200 u    # and back where it was, for what follows
sleep 1

# --- 4. Clipboard ----------------------------------------------------------
eclip() { as_e DISPLAY="$EDPY" XAUTHORITY="$EAUTH" timeout 5 xclip "$@"; }
[ "$(owner)" != "OWNER 0x0" ] && pass "the user's key in it offered it the session's clipboard" || fail "no offer after the user's key"
out=$(eclip -o -selection clipboard 2>/dev/null)
[ "$out" = fromsession ] && pass "after the user's key in it, the session's text can be pasted there" \
    || fail "the user's key did not bring the session's text [$out]"
kill "$XCPID" 2>/dev/null; XCPID=""
printf 'fromelevated' | eclip -selection clipboard -i &
sleep 1
type_keys -M alt -k Tab -m alt    # the user goes back to the session to paste
sleep 1
out=$(DISPLAY="$UD" timeout 5 xclip -o -selection clipboard 2>/dev/null)
[ "$out" = fromelevated ] && pass "text copied in it can be pasted in the session" || fail "elevated -> session copy [$out]"
head -c 64 /dev/urandom > "$T/blob"
DISPLAY="$UD" xclip -selection clipboard -t image/png -i "$T/blob" & XCPID=$!
sleep 1
ctl "ACTIVATE ${EDPY#:} $WIN" >/dev/null
sleep 1
type_keys "i"
sleep 1
if eclip -o -selection clipboard -t TARGETS 2>/dev/null | grep -q 'image/png'; then fail "an image crossed into the elevated display"
else pass "only text crosses: an image copied in the session does not"; fi
kill "$XCPID" 2>/dev/null; XCPID=""

# --- 5. A lock hides it and keeps its keys ---------------------------------
[ "$(ctl LOCK)" = "OK locked" ] || fail "LOCK refused"
sleep 1
r=$(row)
case "$r" in *" hidden "*) pass "locked: the elevated window is hidden" ;; *) fail "locked: [$r]" ;; esac
# The session tries to bring it forward while locked (the taskbar's request).
ctl "ACTIVATE ${EDPY#:} $WIN" >/dev/null
sleep 1
case "$(row)" in *" hidden - "*) pass "locked: ACTIVATE neither shows nor focuses it" ;; *) fail "locked, after ACTIVATE: [$(row)]" ;; esac
k0=$(keys)
type_keys "locked"
sleep 1
[ "$(( $(keys) - k0 ))" -eq 0 ] && pass "locked: it gets no keys" || fail "locked: it got $(( $(keys) - k0 )) keys"
[ "$(ectl UNLOCK)" = "OK unlocked" ] || fail "UNLOCK refused"
sleep 1
case "$(row)" in *" shown "*) pass "unlocked: it is back" ;; *) fail "unlocked: [$(row)]" ;; esac

# --- 6. Who may hand one over; the display ends with the program -----------
refused=$(python3 - "$T/ctl.sock" <<'PY'
import socket, sys, os, array
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
a, b = socket.socketpair(); r, w = os.pipe(); c, d = socket.socketpair()
s.sendmsg([b"ELEVATED\n"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [a.fileno(), c.fileno(), r]))])
print(s.recv(64).decode().strip())
PY
)
[ "$refused" = "ERR not permitted" ] && grep -q "audit: refused ELEVATED from uid $(id -u)" "$T/comp.log" \
    && pass "the session's account cannot hand over a display (refused, audited)" || fail "ELEVATED from the session: [$refused]"
XPID=$(pgrep -u "$EUSER" -f "Xwayland -rootless -wm" | head -1)
sudo -n pkill -u "$EUSER" -f "$T/fx target"
_w=0; while kill -0 "$EPID" 2>/dev/null && [ $_w -lt 30 ]; do sleep 0.5; _w=$((_w+1)); done
if ! kill -0 "$EPID" 2>/dev/null && { [ -z "$XPID" ] || ! kill -0 "$XPID" 2>/dev/null; }; then pass "the display went with the program (Xwayland gone)"
else fail "the display outlived the program"; fi
sleep 1
grep -q "audit: elevated display $EDPY closed" "$T/comp.log" && [ "$(ctl WINDOWS)" = END ] \
    && pass "the compositor let it go" || fail "the compositor still has it: $(ctl WINDOWS | tr '\n' ' ')"
sudo -n test -e "$(dirname "$EAUTH")" && fail "its cookie was left behind" || pass "its cookie is gone"
DISPLAY="$UD" xdpyinfo >/dev/null 2>&1 && pass "the session's own display is unharmed" || fail "the session's display broke"

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
