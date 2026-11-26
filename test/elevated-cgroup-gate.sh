#!/bin/sh
# Elevated programs on the privileged socket and the lock's commands (-E, i0635).
#
# An elevated program runs as the machine account (-U, sgsystem), as the
# lock screen and the consent prompt do, so by uid it could connect to the
# privileged socket -- its window then showed over the locked session -- and
# send the lock's own commands (UNLOCK without a password, SECURE, SAS ...).
# On Windows an elevated program lives on the user's desktop and never
# reaches the secure desktop. sg-brokerd now runs elevated programs in a
# control group of their own (sg-brokerd.service/elevated); -E names it, and
# a machine-account process there is refused the privileged socket and the
# lock's commands. It may still hand over its own display (ELEVATED) and use
# what anyone in the session may (ACTIVATE ...). The machine account
# elsewhere (here: the lock screen's stand-in) and root keep everything.
#
# Needs root and systemd (it runs the clients as transient system services):
# run in the QA VM, never on a host with someone's session. 77 = skipped.
# Mutant: SG_MUTANT_ELEVATED_ANY_CGROUP (lock.c).
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
for t in python3 systemd-run runuser; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
SYS_UID=$(id -u "$SYS_USER" 2>/dev/null) || { echo "SKIP: no $SYS_USER"; exit 77; }
T=$(mktemp -d /var/tmp/sg-elevcg-gate.XXXXXX); chmod 755 "$T"; chown "$SESS_USER" "$T"
mkdir -m 700 "$T/run"; chown "$SESS_USER" "$T/run"
UNITS="sg-elevcggate-el sg-elevcggate-lock"
# shellcheck disable=SC2317  # invoked via trap
cleanup() {
    [ -n "$CP" ] && kill "$CP" 2>/dev/null
    for u in $UNITS; do systemctl stop "$u" 2>/dev/null; systemctl reset-failed "$u" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT INT TERM
EL_CG=/system.slice/sg-elevcggate-el.service

runuser -u "$SESS_USER" -- env XDG_RUNTIME_DIR="$T/run" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$SYS_UID" -R /system.slice/sg-nothing-here.service \
    -E "/system.slice/sg-elevcggate-none.service,$EL_CG" -- \
    sh -c "echo \$WAYLAND_DISPLAY > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while { [ ! -s "$T/d" ] || [ ! -S "$T/priv.sock" ]; } && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
[ -S "$T/priv.sock" ] || { echo "FAIL  the compositor did not start"; cat "$T/log"; exit 1; }
chmod 755 "$T/run" 2>/dev/null
chmod 777 "$T/ctl.sock" "$T/priv.sock" 2>/dev/null

cat > "$T/probe.py" <<'EOF'
import socket, struct, sys
mode, path = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.settimeout(3)
try:
    s.connect(path)
    if mode == "priv":
        # wl_display.get_registry + sync: a compositor that took us answers
        s.sendall(struct.pack("<IHHI", 1, 1, 12, 2) + struct.pack("<IHHI", 1, 0, 12, 3))
        print("accepted" if s.recv(4096) else "refused")
    else:
        s.sendall(sys.argv[3].encode() + b"\n")
        print(s.recv(256).decode().strip() or "closed")
except (ConnectionResetError, BrokenPipeError): print("refused")
except socket.timeout: print("timeout")
EOF
chmod 644 "$T/probe.py"
# as the machine account, in a system service of that name
as_unit() { _u=$1; shift; systemd-run -q --wait --pipe --collect --unit="$_u" -p User="$SYS_USER" python3 "$T/probe.py" "$@" 2>/dev/null; }
el() { as_unit sg-elevcggate-el "$@"; }
lk() { as_unit sg-elevcggate-lock "$@"; }
st() { python3 "$T/probe.py" ctl "$T/ctl.sock" STATUS; }

r=$(lk priv "$T/priv.sock")
[ "$r" = accepted ] && pass "the machine account elsewhere (the lock screen) connects privileged" || fail "the lock screen's stand-in: $r"
r=$(el priv "$T/priv.sock")
[ "$r" = refused ] && pass "an elevated program (the machine account in $EL_CG) is refused the privileged socket" \
    || fail "an elevated program connected privileged: $r"
grep -q "privileged socket refused uid $SYS_UID: an elevated program" "$T/log" && pass "the refusal is audited" \
    || fail "no audit line for the elevated program's refusal"
[ "$(WAYLAND_DISPLAY="$T/priv.sock" python3 "$T/probe.py" priv "$T/priv.sock")" = accepted ] \
    && pass "root connects privileged" || fail "root was refused"

python3 "$T/probe.py" ctl "$T/ctl.sock" LOCK >/dev/null
[ "$(st)" = "OK locked" ] || fail "the gate could not lock the session ($(st))"
r=$(el ctl "$T/ctl.sock" UNLOCK)
[ "$r" = "ERR not permitted" ] && [ "$(st)" = "OK locked" ] \
    && pass "an elevated program's UNLOCK is refused: the session stays locked" \
    || fail "an elevated program's UNLOCK: '$r', $(st)"
for c in SECURE "SAS cancel" WATCH RELEASE LOCAL; do
    r=$(el ctl "$T/ctl.sock" "$c")
    [ "$r" = "ERR not permitted" ] && pass "an elevated program's $c is refused" || fail "an elevated program's $c: '$r'"
done
r=$(el ctl "$T/ctl.sock" ELEVATED)
[ "$r" = "ERR need three descriptors" ] && pass "an elevated program may still hand over its own display (ELEVATED)" \
    || fail "ELEVATED from an elevated program: '$r'"
r=$(el ctl "$T/ctl.sock" "ACTIVATE 1 1")
[ "$r" != "ERR not permitted" ] && pass "and use what anyone in the session may (ACTIVATE: '$r')" \
    || fail "an elevated program's ACTIVATE is refused"
r=$(lk ctl "$T/ctl.sock" UNLOCK)
[ "$r" = "OK unlocked" ] && [ "$(st)" = "OK unlocked" ] && pass "the machine account elsewhere (the lock screen) unlocks" \
    || fail "the lock screen's stand-in's UNLOCK: '$r', $(st)"
grep -q "refused UNLOCK from uid $SYS_UID" "$T/log" && pass "the refused UNLOCK is audited" || fail "no audit line for the refused UNLOCK"

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
