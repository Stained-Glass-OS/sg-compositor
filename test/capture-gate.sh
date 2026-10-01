#!/bin/sh
# Screen capture for everyone, the secure screens blanked (public_capture.c;
# David: "let any app capture, with lock and consent screens blanked out of
# captures"). An ordinary client (grim on the session's socket) captures the
# screen as shown -- a red window -- but black while a secure prompt is up or
# the session is locked; the privileged client still sees the dimmed desktop
# (Remote Desktop must). After the prompt and the lock, the picture is back.
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
for t in grim convert xterm Xwayland python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; echo \"\$WAYLAND_DISPLAY\" > $T/wl; xterm -geometry 200x60+0+0 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
PUB="$T/$(cat "$T/wl" 2>/dev/null)"
[ -S "$PUB" ] || PUB="$(cat "$T/wl" 2>/dev/null)"
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n'); print(s.recv(64).decode().strip())"; }
# the share of pure red pixels, and of pure black ones, in a capture
cap() { rm -f "$T/c.png"; WAYLAND_DISPLAY="$1" grim "$T/c.png" >>"$T/grim.log" 2>&1 || { echo "nocapture"; return; }
        convert "$T/c.png" -scale 64x64! txt:- | awk 'NR > 1 { n++; if ($0 ~ /#FF0000/) r++; if ($0 ~ /#000000/) b++ } END { printf "red=%d black=%d of %d", r, b, n }'; }
pub=$(cap "$PUB"); priv=$(cap "$T/priv.sock")
echo "      unlocked: public $pub; privileged $priv"
case "$pub" in red=0*|nocapture) fail "an ordinary client cannot capture the screen ($pub)";; *) pass "an ordinary client captures the screen as shown ($pub)";; esac
echo "      SECURE: $(ctl SECURE)"; sleep 1.5
pub=$(cap "$PUB"); priv=$(cap "$T/priv.sock")
echo "      secure: public $pub; privileged $priv"
case "$pub" in "red=0 black=4096 of 4096") pass "while a secure prompt is up, its capture is black";; *) fail "secure prompt captured: $pub";; esac
case "$priv" in *"black=4096"*|nocapture) fail "the privileged capture lost the screen: $priv";; *) pass "the privileged client (Remote Desktop) still sees the screen";; esac
echo "      RELEASE: $(ctl RELEASE)"; sleep 1.5
pub=$(cap "$PUB")
case "$pub" in red=0*|nocapture) fail "after the prompt: $pub";; *) pass "after the prompt, the picture is back ($pub)";; esac
echo "      LOCK: $(ctl LOCK)"; sleep 1.5
pub=$(cap "$PUB")
case "$pub" in "red=0 black=4096 of 4096") pass "while locked, its capture is black";; *) fail "lock captured: $pub";; esac
echo "      UNLOCK: $(ctl UNLOCK)"; sleep 1.5
pub=$(cap "$PUB")
case "$pub" in red=0*|nocapture) fail "after unlocking: $pub";; *) pass "after unlocking, the picture is back";; esac
[ -n "${CAPTURE_DEBUG:-}" ] && { grep -i "public capture" "$T/log" | tail -5; tail -3 "$T/grim.log"; }
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
