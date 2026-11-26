#!/bin/sh
# Remote support (ADR 0010 pattern A): who on the privileged socket may
# capture the screen and inject input, and the frame that says so.
#
# The machine account (-U, sgsystem) connects privileged for several things:
# the lock screen, the consent prompt, the RDP pieces -- and, being SYSTEM,
# every elevated program too. Only remote support -- the Windows services and
# the helpers they start, in the control groups -R names -- and root are
# offered capture and injection. Anything else privileged keeps its place
# (seen and focusable while locked) but gets the public capture (lock screen
# black) and no virtual keyboard or pointer. The control group comes from the
# kernel (SO_PEERPIDFD + /proc), and only root can put a process into a
# system service's group, so an elevated program cannot move itself there.
#
# While remote support captures, the amber frame round the screen shows (as
# for the console shadow), and it goes a few seconds after the last capture.
#
# Needs root and systemd (it runs the clients as transient system services):
# run in the QA VM, never on a host with someone's session. 77 = skipped.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
SYS_USER="${SG_SYSTEM_USER:-sgsystem}"
SESS_USER="${SG_SUPPORT_SESSION_USER:-nobody}"
RC=0; CP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
[ "$(id -u)" -eq 0 ] || { echo "SKIP: needs root (QA VM)"; exit 77; }
[ -d /run/systemd/system ] || { echo "SKIP: needs systemd"; exit 77; }
for t in wayland-info grim python3 systemd-run runuser; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
SYS_UID=$(id -u "$SYS_USER" 2>/dev/null) || { echo "SKIP: no $SYS_USER"; exit 77; }
T=$(mktemp -d /var/tmp/sg-support-gate.XXXXXX); chmod 755 "$T"; chown "$SESS_USER" "$T"
mkdir -m 700 "$T/run"; chown "$SESS_USER" "$T/run"
# shellcheck disable=SC2317  # invoked via trap
cleanup() {
    [ -n "$CP" ] && kill "$CP" 2>/dev/null
    for u in sg-supportgate-ok sg-supportgate-other; do systemctl stop "$u" 2>/dev/null; systemctl reset-failed "$u" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT INT TERM
OK_CG=/system.slice/sg-supportgate-ok.service

runuser -u "$SESS_USER" -- env XDG_RUNTIME_DIR="$T/run" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$SYS_UID" -R "/system.slice/sg-nothing-here.service,$OK_CG" -- \
    sh -c "echo \$WAYLAND_DISPLAY > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while { [ ! -s "$T/d" ] || [ ! -S "$T/priv.sock" ]; } && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
[ -S "$T/priv.sock" ] || { echo "FAIL  the compositor did not start"; cat "$T/log"; exit 1; }

# as the machine account, in a system service of that name
as_unit() { _u=$1; shift; systemd-run -q --wait --pipe --collect --unit="$_u" -p User="$SYS_USER" \
                -E WAYLAND_DISPLAY="$T/priv.sock" -E XDG_RUNTIME_DIR=/tmp "$@" 2>/dev/null; }
SENS='zwlr_export_dmabuf_manager_v1|zwp_virtual_keyboard_manager_v1|zwlr_virtual_pointer_manager_v1|zwlr_gamma_control_manager_v1'
as_unit sg-supportgate-ok wayland-info >"$T/ok.info"
as_unit sg-supportgate-other wayland-info >"$T/other.info"
WAYLAND_DISPLAY="$T/priv.sock" XDG_RUNTIME_DIR=/tmp wayland-info >"$T/root.info" 2>/dev/null
n=$(grep -oE "$SENS" "$T/ok.info" | sort -u | wc -l)
[ "$n" -eq 4 ] && pass "remote support (the machine account in $OK_CG) is offered injection, dmabuf and gamma" \
               || fail "remote support is offered $n of the four"
n=$(grep -cE "$SENS" "$T/other.info")
[ "$n" -eq 0 ] && pass "the machine account elsewhere (an elevated program, the lock screen) is offered none of them" \
               || fail "the machine account outside -R is offered $n of them"
n=$(grep -oE "$SENS" "$T/root.info" | sort -u | wc -l)
[ "$n" -eq 4 ] && pass "root is offered all four" || fail "root is offered $n of four"
sc_name() { awk -F"name: " '/zwlr_screencopy_manager_v1/ { split($2, a, /[^0-9]/); print a[1]; exit }' "$1"; }
[ -n "$(sc_name "$T/ok.info")" ] && [ "$(sc_name "$T/ok.info")" = "$(sc_name "$T/root.info")" ] \
    && pass "remote support gets wlroots' screen capture, as root does" || fail "remote support's capture is not the real one"
[ -n "$(sc_name "$T/other.info")" ] && [ "$(sc_name "$T/other.info")" != "$(sc_name "$T/ok.info")" ] \
    && pass "the machine account elsewhere gets only the public capture" || fail "the machine account outside -R gets the real capture"
grep -q "accepted uid $SYS_UID: remote support" "$T/log" && pass "remote support's connection is audited" || fail "no audit line for remote support"
grep -q "accepted uid $SYS_UID, without capture" "$T/log" && pass "and so is a privileged connection without capture" || fail "no audit line for the refused capture"

# Pixels: (2,2) is the frame's corner; the session shows nothing there.
corner() { python3 -c 'import sys
d=sys.stdin.buffer.read()
if not d.startswith(b"P6"): print("none"); sys.exit()
import re
m=re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d); w=int(m.group(1)); px=d[m.end():]
o=(2*w+2)*3; print("%02x%02x%02x" % (px[o],px[o+1],px[o+2]))'; }
ctl() { python3 -c 'import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(sys.argv[2].encode() + b"\n"); print(s.recv(64).decode().strip())' "$T/ctl.sock" "$1"; }
root_corner() { WAYLAND_DISPLAY="$T/priv.sock" XDG_RUNTIME_DIR=/tmp grim -t ppm - 2>/dev/null | corner; }
c=$(root_corner)
[ "$c" != none ] && [ "$c" != ffb400 ] && pass "no frame before remote support captures (corner $c)" || fail "frame before any remote-support capture ($c)"
c=$(as_unit sg-supportgate-ok grim -t ppm - | corner)
[ "$c" = ffb400 ] && pass "remote support captures, and its picture shows the frame" || fail "remote support's capture: corner $c"
c=$(root_corner)
[ "$c" = ffb400 ] && pass "the frame shows on the screen while remote support captures" || fail "no frame after a remote-support capture ($c)"
grep -q "remote support is capturing the screen" "$T/log" && pass "the capture is audited" || fail "the capture is not audited"
python3 -c 'import time; time.sleep(4.5)'
c=$(root_corner)
[ "$c" != ffb400 ] && pass "the frame goes a few seconds after the last capture" || fail "the frame stayed"
c=$(as_unit sg-supportgate-other grim -t ppm - | corner)
python3 -c 'import time; time.sleep(0.3)'
[ "$(root_corner)" != ffb400 ] && pass "a capture outside -R (the public one) shows no frame" || fail "a public capture showed the frame"

# Locked: remote support sees the screen (here the frame on a locked,
# empty screen), the machine account elsewhere a black picture.
ctl LOCK >/dev/null; python3 -c 'import time; time.sleep(0.5)'
c=$(as_unit sg-supportgate-other grim -t ppm - | corner)
[ "$c" = 000000 ] && pass "while locked, the machine account outside -R captures black" || fail "while locked, outside -R captured $c"
c=$(as_unit sg-supportgate-ok grim -t ppm - | corner)
[ "$c" = ffb400 ] && pass "while locked, remote support still captures the screen" || fail "while locked, remote support captured $c"
ctl UNLOCK >/dev/null

# The session's own account (any account but -U and root): not privileged.
probe='
import socket, struct, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(3)
try:
    s.connect(sys.argv[1]); s.sendall(struct.pack("<IHHI", 1, 1, 12, 2) + struct.pack("<IHHI", 1, 0, 12, 3))
    print("accepted" if s.recv(4096) else "refused")
except (ConnectionResetError, BrokenPipeError): print("refused")
except socket.timeout: print("timeout")'
[ "$(runuser -u "$SESS_USER" -- python3 -c "$probe" "$T/priv.sock")" = refused ] \
    && pass "the session's own account cannot connect privileged" || fail "the session's account connected privileged"

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
