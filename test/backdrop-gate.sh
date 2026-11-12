#!/bin/sh
# The backdrop (backdrop.c): where no window is, the colour and the centred
# picture of the backdrop file, not black -- the first session after setup,
# and every sign-in until the desktop is drawn, showed a black screen for up to
# a minute. Without the file, the colour alone (purple). Once the session's
# desktop (Wine's "shell - Wine Desktop" window) is up, the picture goes and
# the colour stays: on a second screen the desktop did not reach, the sign-in
# words stayed for good (David 2026-11-12). Mutant: SG_MUTANT_BACKDROP_STAYS.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim python3 convert; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }

# a backdrop: colour 0xff20a040, a 40x20 picture of 0xffe01010
python3 - "$T/b.sgbd" <<'EOF'
import struct, sys
w, h = 40, 20
with open(sys.argv[1], "wb") as f:
    f.write(b"SGBD" + struct.pack("<III", w, h, 0xff20a040))
    f.write(struct.pack("<I", 0xffe01010) * (w * h))
EOF

run() {   # backdrop file (or none) [desktop] -> $T/c.png; "desktop": a small shell desktop window first
    rm -f "$T/d" "$T/c.png"
    _client="exec sleep 600"
    [ "${2:-}" = desktop ] && _client="exec xterm -class explorer.exe -T 'shell - Wine Desktop' -geometry 10x3+0+0 -e sleep 600"
    SG_BACKDROP="$1" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
        "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
        sh -c "echo \$WAYLAND_DISPLAY > $T/d; echo \$\$ > $T/client; $_client" >"$T/log" 2>&1 &
    CP=$!
    _w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
    sleep 1
    [ "${2:-}" = desktop ] && sleep 3   # Xwayland, and the window mapped
    WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
    # the session ends when its client does
    kill "$(cat "$T/client")" 2>/dev/null; wait "$CP" 2>/dev/null; CP=
    rm -f "$T/priv.sock" "$T/ctl.sock"
}
px() { convert "$T/c.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
size() { convert "$T/c.png" -format "%w %h" info: 2>/dev/null; }

run "$T/b.sgbd"
[ -s "$T/c.png" ] || { fail "no capture"; cat "$T/log"; exit 1; }
set -- $(size); W=$1; H=$2
[ "$(px 5 5)" = "32,160,64" ] && pass "where no window is: the backdrop's colour" || fail "corner: $(px 5 5)"
[ "$(px $((W / 2)) $((H / 2)))" = "224,16,16" ] && pass "and its picture, centred" || fail "centre: $(px $((W / 2)) $((H / 2)))"
if command -v xterm >/dev/null; then
    run "$T/b.sgbd" desktop
    [ "$(px $((W / 2)) $((H / 2)))" = "32,160,64" ] && pass "the desktop is up: the picture goes, the colour stays where it does not reach" ||
        fail "the picture stays after the desktop came up: centre $(px $((W / 2)) $((H / 2)))"
else
    echo "SKIP  the desktop case (no xterm)"
fi
run "$T/none.sgbd"
[ "$(px 5 5)" = "42,22,76" ] && pass "without the file: purple, not black" || fail "no file: $(px 5 5)"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
