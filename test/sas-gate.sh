#!/bin/sh
# Reserved keys: Win+L and the secure attention sequence (Ctrl+Alt+Del).
#
# Both must lock, and neither may reach any client -- not the press, and not
# the release either, which would tell the client a reserved key was used.
# Ordinary keys must still arrive, or "nothing reached the client" proves
# nothing.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
RC=0; T=$(mktemp -d); chmod 755 "$T"; CP=""
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
# shellcheck disable=SC2317  # invoked via trap
cleanup() { [ -n "$CP" ] && kill "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in wtype xev xdotool python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$DISPLAY > $T/xd; exec xev -event keyboard" >"$T/xev.txt" 2>"$T/log" &
CP=$!
_w=0; while [ ! -s "$T/xd" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 2
XD=$(cat "$T/xd")
W=$(DISPLAY="$XD" xdotool search --name 'Event Tester' 2>/dev/null | head -1)
focus() { DISPLAY="$XD" xdotool windowfocus "$W" 2>/dev/null; sleep 1; }
inj() { WAYLAND_DISPLAY="$T/priv.sock" wtype "$@"; sleep 1; }
ctl() { python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('$T/ctl.sock');s.sendall(b'$1\n');print(s.recv(64).decode().strip())"; }
presses() { awk '/^KeyPress/{p=1;next} p&&match($0,/keysym 0x[0-9a-f]+, [A-Za-z_0-9]+\)/){s=substr($0,RSTART,RLENGTH); sub(/.*, /,"",s); sub(/\)/,"",s); print s; p=0}' "$T/xev.txt"; }
releases() { awk '/^KeyRelease/{p=1;next} p&&match($0,/keysym 0x[0-9a-f]+, [A-Za-z_0-9]+\)/){s=substr($0,RSTART,RLENGTH); sub(/.*, /,"",s); sub(/\)/,"",s); print s; p=0}' "$T/xev.txt"; }

# The first injected keys can be lost while XWayland takes on the virtual
# keyboard's keymap and X focus settles. Keep trying until an ordinary key is
# seen to arrive: that is the teeth check, and the real checks run only after it,
# so a pass or fail below is about reserved keys and never about warm-up.
tries=0
until presses | grep -qx b; do
    tries=$((tries + 1))
    [ "$tries" -gt 8 ] && break
    focus; inj b
done
presses | grep -qx b && pass "ordinary keys reach the client (the gate has teeth)" \
    || { fail "an ordinary key never arrived -- result meaningless"; echo "RESULT: FAIL"; exit 1; }

inj -M logo l -m logo
[ "$(ctl STATUS)" = "OK locked" ] && pass "Win+L locks" || fail "Win+L did not lock"
ctl UNLOCK >/dev/null; focus

inj -M ctrl -M alt -k Delete -m alt -m ctrl
[ "$(ctl STATUS)" = "OK locked" ] && pass "Ctrl+Alt+Del locks" || fail "Ctrl+Alt+Del did not lock"
ctl UNLOCK >/dev/null; focus

inj c
presses | grep -qx c && pass "input resumes after unlock" || fail "input did not resume after unlock"

if presses | grep -qxE 'l|L|Delete|KP_Delete'; then fail "a reserved key's press reached the client"
else pass "no reserved key's press reached the client"; fi
if releases | grep -qxE 'l|L|Delete|KP_Delete'; then fail "a reserved key's release reached the client"
else pass "no reserved key's release reached the client"; fi

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
