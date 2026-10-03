#!/bin/sh
# A Linux program's title bar (decor.c) is the Wine frames' own: the look and
# sizes Settings writes to effects.conf with the window style -- the window
# goes into a Wine frame a moment after it maps, and the two looked
# different (David 2026-10-02: "Linux apps seem to get smaller window
# decorations"; the terminal "starts with one window decorator, then
# changes a bit later"). And while the taskbar is framing Linux windows (it
# asks for XWINDOWS), a new one is held back, unseen, until it is framed or
# a moment (2.5 s) has passed.
#   1. frame=horizon caption=25 border=4: a blue bar 28 px tall (the frame's
#      3 px inside its outer line, and the caption), 3 px of blue frame at
#      the window's sides
#   2. frame=glass caption=30 border=8 glass=60: a 37 px bar, see-through
#      (the Glass frame's pale blue mixed with the screen's backdrop)
#   3. the taskbar asking for XWINDOWS: a new window is not shown at first,
#      and shown once the moment has passed unframed
#   4. nobody asking: a new window is shown at once
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; CP=
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
cleanup() { [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null; [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert xterm Xwayland python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
export XDG_CONFIG_HOME="$T/cfg"
mkdir -p "$XDG_CONFIG_HOME/stained-glass"
conf() { printf 'frame=%s\ncaption=%s\nbutton=%s\nbuttonh=%s\nborder=%s\nscale8=8\nglass=%s\n' "$@" > "$XDG_CONFIG_HOME/stained-glass/effects.conf"; }
conf horizon 25 25 25 4 0
# one red terminal at a time, started when asked (go.N), centred
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; echo up > $T/d; for n in 1 2 3 4; do while [ ! -f $T/go.\$n ]; do sleep 0.05; done; xterm -geometry 40x8 -bg '#ff0000' -fg '#ff0000' -e sleep 600 & echo \$! > $T/xt.\$n; done; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 2
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n')
d=b''
while True:
    c=s.recv(65536)
    if not c: break
    d+=c
print(d.decode(), end='')"; }
shot() { rm -f "$T/c.png"; WAYLAND_DISPLAY="$T/priv.sock" grim "$T/c.png" >/dev/null 2>&1; }
px() { convert "$T/c.png" -format "%[fx:int(255*p{$1,$2}.r)],%[fx:int(255*p{$1,$2}.g)],%[fx:int(255*p{$1,$2}.b)]" info: 2>/dev/null; }
is() { px "$1" "$2" | awk -F, "{ r = \$1; g = \$2; b = \$3; exit !($3) }"; }
# the red window: its left edge and top on its middle row, and the bar's
# top (its outer line's row: the first row that is not the bar going up),
# read from one row and one column of the picture
row_of() { convert "$T/c.png" -crop "${W}x1+0+$1" +repage txt:- 2>/dev/null | awk -F'[,:]' 'NR > 1 { print $1, $0 }' | sed 's/ [0-9]*,0: (\([0-9]*\),\([0-9]*\),\([0-9]*\).*/ \1 \2 \3/'; }
col_of() { convert "$T/c.png" -crop "1x${H}+$1+0" +repage txt:- 2>/dev/null | awk 'NR > 1' | sed 's/^0,\([0-9]*\): (\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1 \2 \3 \4/'; }
measure() {
    shot; set -- $(convert "$T/c.png" -format "%w %h" info:); W=$1; H=$2; row=$((H / 2)); L=0; TOP=0; BT=0
    L=$(row_of "$row" | awk '$2 == 255 && $3 == 0 && $4 == 0 { print $1; exit }')
    [ -n "$L" ] && [ "$L" -gt 0 ] || { L=0; return 1; }
    col=$((L + 40))
    col_of "$col" > "$T/col"
    TOP=$(awk -v r="$row" '$1 <= r { if ($2 == 255 && $3 == 0 && $4 == 0) top = $1; else if (top) last = top } END { print last ? last : top }' "$T/col")
    TOP=$(awk -v r="$row" '{ red[$1] = ($2 == 255 && $3 == 0 && $4 == 0) } END { y = r; while (y > 0 && red[y - 1]) y--; print y }' "$T/col")
    # up from the window's top while the row is not the screen's backdrop
    # (the column's first row): the last such row is the outer line, the
    # bar is below it
    BT=$(awk -v t="$TOP" '{ c[$1] = $2 " " $3 " " $4 } END { y = t - 1; while (y > 0 && c[y - 1] != c[0]) y--; print y + 1 }' "$T/col")
}
kill_xt() { [ -s "$T/xt.$1" ] && kill "$(cat "$T/xt.$1")" 2>/dev/null; sleep 1; }

# 1. Horizon, 25 + 4
touch "$T/go.1"; sleep 3
if measure; then
    bh=$((TOP - BT))
    is $((col + 70)) $((TOP - 10)) 'b > r + 100' && [ "$bh" -ge 28 ] && [ "$bh" -le 29 ] \
        && pass "Horizon: a blue bar $bh px tall ($(px $((col + 70)) $((TOP - 10))))" || fail "Horizon bar: $bh px from $BT to $TOP, $(px $((col + 70)) $((TOP - 10)))"
    is $((L - 2)) "$row" 'b > r + 100' && pass "and the blue frame at its side ($(px $((L - 2)) "$row"))" || fail "Horizon side: $(px $((L - 2)) "$row")"
else fail "no window (1)"; fi
cp "$T/c.png" "$T/horizon.png"
kill_xt 1

# 2. Glass, 30 + 8, see-through
conf glass 30 30 30 8 60
touch "$T/go.2"; sleep 3
if measure; then
    bh=$((TOP - BT))
    v=$(px "$col" $((TOP - 12)))
    is "$col" $((TOP - 12)) 'b > r + 20 && r < 150' && [ "$bh" -ge 37 ] && [ "$bh" -le 38 ] \
        && pass "Glass: a see-through pale blue bar $bh px tall ($v, over the screen's dark backdrop)" || fail "Glass bar: $bh px, $v"
else fail "no window (2)"; fi
cp "$T/c.png" "$T/glass.png"
[ -n "${SHOTS:-}" ] && cp "$T/horizon.png" "$SHOTS/decor-horizon.png" && cp "$T/glass.png" "$SHOTS/decor-glass.png"
kill_xt 2

# 3. the taskbar asking for the list: held back, then shown unframed
ctl XWINDOWS > /dev/null
( for i in $(seq 1 12); do ctl XWINDOWS > /dev/null; sleep 0.5; done ) & ASK=$!
touch "$T/go.3"; sleep 1.2
shot; held=$(convert "$T/c.png" -fill black +opaque '#ff0000' -fill white -opaque '#ff0000' -format "%[fx:int(mean*w*h+0.5)]" info:)
sleep 3
shot; shown=$(convert "$T/c.png" -fill black +opaque '#ff0000' -fill white -opaque '#ff0000' -format "%[fx:int(mean*w*h+0.5)]" info:)
[ "${held:-1}" -eq 0 ] && [ "${shown:-0}" -gt 1000 ] \
    && pass "while the taskbar frames windows a new one is held back ($held red pixels at 1.2 s), then shown ($shown)" \
    || fail "hold: $held red pixels at 1.2 s, $shown at 4 s"
wait "$ASK"
kill_xt 3

# 4. nobody asking (the taskbar gone): shown at once
sleep 3.5
touch "$T/go.4"; sleep 1.2
shot; now=$(convert "$T/c.png" -fill black +opaque '#ff0000' -fill white -opaque '#ff0000' -format "%[fx:int(mean*w*h+0.5)]" info:)
[ "${now:-0}" -gt 1000 ] && pass "with no taskbar framing, a new window shows at once ($now red pixels)" || fail "not shown: $now"
kill_xt 4

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
