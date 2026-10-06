#!/bin/sh
# A Linux program's Horizon and Glass caption buttons (decor.c) are drawn
# smooth: the close button's cross was put pixel by pixel along two
# diagonals, and the buttons' and the bar's rounded corners were in or out
# per pixel -- stepped (David 2026-10-06: review all the icons we draw and
# get them rendered at a better resolution). The cross is drawn by each
# pixel's share of its two strokes, the corners by each pixel's share of the
# rounded outline (4 x 4 samples). Under a headless compositor, a red xterm
# in the Horizon look:
#   1. along any row through the close button's cross, at least 4 colours
#      (the button's, the cross's and blends; stepped: 2)
#   2. at the close button's rounded top-left corner, at least 11 colours
#      (stepped: 8)
# Mutant: SG_MUTANT_JAGGED_DECOR (decor.c) fails both.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim xterm Xwayland python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3-pil missing"; exit 77; }
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
export XDG_CONFIG_HOME="$T/cfg"
mkdir -p "$XDG_CONFIG_HOME/stained-glass"
printf 'frame=horizon\ncaption=25\nbutton=25\nbuttonh=25\nborder=4\nscale8=8\nglass=0\n' > "$XDG_CONFIG_HOME/stained-glass/effects.conf"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; echo up > $T/d; xterm -geometry 40x8 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 4
WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1
[ -s "$T/c.png" ] || { fail "no screenshot"; exit 1; }
[ -n "${SHOTS:-}" ] && cp "$T/c.png" "$SHOTS/decorsmooth.png"
set -- $(python3 - "$T/c.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); w, h = im.size; px = im.load()
reds = [(x, y) for y in range(h) for x in range(w) if px[x, y] == (255, 0, 0)]
if not reds: print("0 0"); sys.exit()
x1 = max(p[0] for p in reds); y0 = min(p[1] for p in reds)
cl = [(x, y) for y in range(max(0, y0 - 45), y0) for x in range(max(0, x1 - 60), min(w, x1 + 20))
      if px[x, y][0] > 180 and px[x, y][1] < 140 and px[x, y][2] < 120]
if not cl: print("0 0"); sys.exit()
bx0 = min(p[0] for p in cl); bx1 = max(p[0] for p in cl); by0 = min(p[1] for p in cl); by1 = max(p[1] for p in cl)
glyph = max(len({px[x, y] for x in range(bx0 + 2, bx1 - 1)}) for y in range(by0 + 2, by1 - 1))
corner = len({px[x, y] for x in range(bx0 - 1, bx0 + 4) for y in range(by0 - 1, by0 + 4)})
print(glyph, corner)
PY
)
[ "${1:-0}" -ge 4 ] && pass "the close button's cross is smooth (up to $1 colours along a row)" || fail "the close button's cross is stepped (${1:-?} colours along a row, want 4 or more)"
[ "${2:-0}" -ge 11 ] && pass "the buttons' rounded corners are smooth ($2 colours at a corner)" || fail "the buttons' corners are stepped (${2:-?} colours, want 11 or more)"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
