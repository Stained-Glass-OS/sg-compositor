#!/bin/sh
# The capture border (capture_border.c): the thin yellow frame Windows draws
# round what Windows.Graphics.Capture captures. A program asks on the control
# socket (CAPTUREBORDER, then RECT x y w h); the compositor draws it on the
# screen -- the privileged capture, which sees what the screen shows, has it
# -- but never in a capture an ordinary program gets (the public screencopy,
# whole or a region); not while a secure prompt is up; OFF and RECT again;
# gone when the connection closes.
#
#   sh test/capborder-gate.sh            (SG_COMPOSITOR_BIN: another build)
# Mutants: SG_MUTANT_BORDER_CAPTURED (public_capture.c: the frame in captures),
# SG_MUTANT_BORDER_STAYS (capture_border.c: it outlives its connection).
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

# the border's connection: a small helper that holds it and takes lines on a fifo
mkfifo "$T/cmd"
python3 - "$T/ctl.sock" "$T/cmd" "$T/replies" <<'EOF' &
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"CAPTUREBORDER\n")
out = open(sys.argv[3], "w", buffering=1)
out.write(s.recv(64).decode())
for line in open(sys.argv[2]):
    line = line.strip()
    if line == "CLOSE":
        break
    s.sendall((line + "\n").encode()); out.write(s.recv(64).decode())
s.close()
EOF
HP=$!
exec 3>"$T/cmd"
say() { echo "$1" >&3; sleep 1.2; }
# a pixel of a capture: PIXEL socket x y [region]
pixel() { rm -f "$T/c.png"; if [ -n "${4:-}" ]; then WAYLAND_DISPLAY="$1" grim -g "$4" "$T/c.png" 2>/dev/null; else WAYLAND_DISPLAY="$1" grim "$T/c.png" 2>/dev/null; fi
          [ -f "$T/c.png" ] && convert "$T/c.png" -crop "1x1+$2+$3" txt:- | sed -n 's/.*\(#[0-9A-F]\{6\}\).*/\1/p' | head -1 || echo none; }
YEL="#FFD800"

say "RECT 100 100 300 200"
grep -q '^OK capture border' "$T/replies" && grep -q '^OK$' "$T/replies" && pass "CAPTUREBORDER and RECT answered OK" || fail "replies: $(cat "$T/replies")"
on=$(pixel "$T/priv.sock" 100 150); mid=$(pixel "$T/priv.sock" 250 200)
[ "$on" = "$YEL" ] && [ "$mid" = "#FF0000" ] && pass "the frame is on the screen, on the captured rectangle's edge (inside red)" || fail "on screen: edge $on, inside $mid"
pub=$(pixel "$PUB" 100 150); pubr=$(pixel "$PUB" 0 50 "100,100 300x200")
[ "$pub" = "#FF0000" ] && [ "$pubr" = "#FF0000" ] && pass "never in a capture an ordinary program gets (the screen, or the rectangle itself)" || fail "captured: whole $pub, region $pubr"
say "RECT 400 300 200 100"
[ "$(pixel "$T/priv.sock" 400 350)" = "$YEL" ] && [ "$(pixel "$T/priv.sock" 100 150)" = "#FF0000" ] && pass "RECT again: it moves with the window" || fail "after moving"
python3 -c "import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'SECURE\n'); print(s.recv(64).decode().strip())" >/dev/null; sleep 1.5
sec=$(pixel "$T/priv.sock" 400 350)
[ "$sec" != "$YEL" ] && pass "not shown while a secure prompt is up ($sec)" || fail "shown over a secure prompt"
python3 -c "import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'RELEASE\n'); print(s.recv(64).decode().strip())" >/dev/null; sleep 1.5
[ "$(pixel "$T/priv.sock" 400 350)" = "$YEL" ] && pass "back after the prompt" || fail "not back after the prompt"
say "OFF"
[ "$(pixel "$T/priv.sock" 400 350)" = "#FF0000" ] && pass "OFF hides it" || fail "OFF"
say "RECT 400 300 200 100"
[ "$(pixel "$T/priv.sock" 400 350)" = "$YEL" ] && pass "RECT shows it again" || fail "RECT after OFF"
say "CLOSE"; exec 3>&-; wait "$HP" 2>/dev/null; sleep 1.5
[ "$(pixel "$T/priv.sock" 400 350)" = "#FF0000" ] && pass "gone when the connection closes" || fail "the frame outlived its connection"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
