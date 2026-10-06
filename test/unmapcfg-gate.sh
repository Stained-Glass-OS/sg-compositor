#!/bin/sh
# A window hidden and asked to move while hidden does not take the
# compositor down. view_unmap destroyed the view's scene tree but kept the
# pointer to it, and every "if (view->scene_tree)" afterwards used freed
# memory: a managed X window asking to be configured after it was unmapped
# (a Wine message box closing: Setup's "additional partitions" box at
# 2736x1824, s14 regression walk 2026-10-06) moved a freed scene node --
# sg-compositor died in libwlroots scene_node_get_root and the login screen
# (Setup) with it. An X client shows, hides and moves a window 60 times; the
# compositor must still run and answer. Run with an AddressSanitizer build
# (SG_COMPOSITOR_BIN=build-asan/sg-compositor, as make test-unmapcfg does)
# any use of the freed tree is reported, so it fails every time without the
# fix (mutant SG_MUTANT_UNMAP_DANGLING_TREE).
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
for t in Xwayland cc python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
cc -O2 -o "$T/unmapcfg-client" "$HERE/test/unmapcfg-client.c" -lX11 || { echo "SKIP: no libX11 headers"; exit 77; }
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=0}"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; echo \"\$DISPLAY\" > $T/dpy; echo up > $T/d; exec sleep 600" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 2
export DISPLAY="$(cat "$T/dpy")"
timeout 120 "$T/unmapcfg-client" 60 > "$T/out" 2>&1
sleep 2
grep -q '^done 60' "$T/out" && pass "60 windows shown, hidden and moved while hidden" || fail "the client: $(tail -2 "$T/out")"
if kill -0 "$CP" 2>/dev/null && ! grep -q 'AddressSanitizer' "$T/log"; then
    pass "the compositor still runs, no use of freed memory"
else
    fail "the compositor: $(grep -m3 -E 'AddressSanitizer|SUMMARY|#[0-3] ' "$T/log" | tr '\n' '|')"
fi
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect('$T/ctl.sock'); s.sendall(b'$1\n')
d=b''
while not d.endswith(b'END\n'):
    c=s.recv(4096)
    if not c: break
    d+=c
print(d.decode())" 2>&1; }
ctl XWINDOWS | grep -q END && pass "and answers its control socket" || fail "no answer on the control socket"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
