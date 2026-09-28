#!/bin/sh
# Session-scoped privileged protocols (ADR 0011 milestone 3).
#
# Screen capture and input injection exist only for privileged clients, and
# who is privileged is decided by the kernel's SO_PEERCRED, not by the client.
# Output management is the exception: every program may set the resolution
# (Display settings does), but not while locked and never all displays off.
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
for t in wayland-info grim python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$WAYLAND_DISPLAY > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
D=$(cat "$T/d")

SENS='zwlr_screencopy_manager_v1|zwlr_export_dmabuf_manager_v1|zwp_virtual_keyboard_manager_v1|zwlr_virtual_pointer_manager_v1|zwlr_gamma_control_manager_v1'
n=$(WAYLAND_DISPLAY="$D" wayland-info 2>/dev/null | grep -cE "$SENS")
[ "$n" -eq 0 ] && pass "an ordinary client is offered none of the five sensitive protocols" \
               || fail "an ordinary client is offered $n sensitive protocols"
n=$(WAYLAND_DISPLAY="$T/priv.sock" wayland-info 2>/dev/null | grep -oE "$SENS" | sort -u | wc -l)
[ "$n" -eq 5 ] && pass "a privileged client is offered all five" || fail "a privileged client is offered $n of five"

# Resolution: any client may, as Display settings does.
if command -v wlr-randr >/dev/null; then
    O=$(WAYLAND_DISPLAY="$D" wlr-randr 2>/dev/null | awk 'NR==1{print $1}')
    WAYLAND_DISPLAY="$D" wlr-randr --output "$O" --custom-mode 1600x900 >/dev/null 2>&1
    WAYLAND_DISPLAY="$D" wlr-randr 2>/dev/null | grep -q '1600x900.*current' \
        && pass "an ordinary client can set the resolution" || fail "an ordinary client could not set the resolution"
    WAYLAND_DISPLAY="$D" wlr-randr --output "$O" --off >/dev/null 2>&1
    WAYLAND_DISPLAY="$D" wlr-randr 2>/dev/null | grep -q 'Enabled: yes' \
        && pass "but not turn every display off" || fail "an ordinary client turned every display off"
    python3 -c 'import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"LOCK\n"); s.recv(64)' "$T/ctl.sock"
    WAYLAND_DISPLAY="$D" wlr-randr --output "$O" --custom-mode 1024x768 >/dev/null 2>&1
    WAYLAND_DISPLAY="$D" wlr-randr 2>/dev/null | grep -q '1600x900.*current' \
        && pass "nor change it while the machine is locked" || fail "an ordinary client changed the resolution while locked"
    WAYLAND_DISPLAY="$T/priv.sock" wlr-randr --output "$O" --custom-mode 1280x720 >/dev/null 2>&1
    WAYLAND_DISPLAY="$T/priv.sock" wlr-randr 2>/dev/null | grep -q '1280x720.*current' \
        && pass "the lock screen still can" || fail "a privileged client could not set the resolution while locked"
    python3 -c 'import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"UNLOCK\n"); s.recv(64)' "$T/ctl.sock"
else
    echo "info  wlr-randr missing: resolution checks skipped"
fi

WAYLAND_DISPLAY="$D" grim "$T/o.png" >/dev/null 2>&1
[ -s "$T/o.png" ] && fail "an ordinary client captured the screen" || pass "an ordinary client cannot capture the screen"
WAYLAND_DISPLAY="$T/priv.sock" grim "$T/p.png" >/dev/null 2>&1
[ -s "$T/p.png" ] && pass "a privileged client can capture the screen" || fail "a privileged client could not capture"

# Across accounts: needs a second identity, so it skips without passwordless sudo.
if sudo -n true 2>/dev/null; then
    probe='
import socket, struct, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(3)
try:
    s.connect(sys.argv[1]); s.sendall(struct.pack("<IHHI", 1, 1, 12, 2) + struct.pack("<IHHI", 1, 0, 12, 3))
    print("accepted" if s.recv(4096) else "refused")
except (ConnectionResetError, BrokenPipeError): print("refused")
except socket.timeout: print("timeout")'
    ctl='
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(sys.argv[2].encode() + b"\n"); print(s.recv(64).decode().strip())'
    [ "$(sudo -u nobody python3 -c "$probe" "$T/priv.sock")" = refused ] \
        && pass "another account cannot connect as privileged" || fail "another account connected as privileged"
    python3 -c "$ctl" "$T/ctl.sock" LOCK >/dev/null
    [ "$(sudo -u nobody python3 -c "$ctl" "$T/ctl.sock" UNLOCK)" = "ERR not permitted" ] \
        && pass "another account cannot UNLOCK" || fail "another account could UNLOCK"
    [ "$(python3 -c "$ctl" "$T/ctl.sock" STATUS)" = "OK locked" ] \
        && pass "and the machine stays locked" || fail "the lock did not hold"
    grep -q 'audit: refused UNLOCK' "$T/log" && pass "the refused UNLOCK is audited" || fail "refused UNLOCK not audited"
else
    echo "info  no passwordless sudo: cross-account checks skipped"
fi

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
