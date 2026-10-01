#!/bin/sh
# sg-deskcomp: the Wine desktop's compositor (drop shadows, real alpha,
# window effects). David: "The apps need to have drop shadows like in
# windows. We also should support alpha in windows." -- and effects in the
# manner of the old compositing desktops, off by default.
#
# A Wine virtual desktop on Xvfb (a flat green desktop), with wine-sg 0744
# (the desktop's picture and the windows' shadow kinds), sg-deskcomp
# compositing it:
#   - a window with a title bar has a shadow below and beside it, and none
#     once sg-deskcomp is gone (the desktop drawn by X again, its canvas gone);
#   - a layered window half opaque is blended over the desktop, and a window
#     with per-pixel alpha too; a frosted one (wine-sg 0745's _SG_ACRYLIC:
#     the taskbar with "Transparency effects") blurs what is below it;
#   - a full-screen window on top is drawn by X itself (no copy, the canvas
#     hidden), and composited again once it is gone;
#   - a new window fades in (open=fade); a dragged window wobbles and settles
#     where it was dropped (wobbly=1); a minimized one shrinks into the
#     taskbar, its picture kept for the animation (minimize=lamp).
#
#   WINE=/opt/wine-sg/bin/wine sh test/deskcomp-gate.sh [--mutants]
#   SG_DESKCOMP=<binary> to test another build (default build/sg-deskcomp).
# --mutants builds sg-deskcomp with each SG_MUTANT_* switch and requires the
# gate to fail with every one.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${WINE:-/opt/wine-sg/bin/wine}"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
DESKCOMP="${SG_DESKCOMP:-$HERE/build/sg-deskcomp}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
unset DISPLAY WAYLAND_DISPLAY

if [ "${1:-}" = --mutants ]; then
    rc=0
    for m in NOSHADOWS OPAQUE NOEFFECTS NOFROST NODIRECT; do
        out=$(mktemp /var/tmp/sg-deskcomp-mutant.XXXXXX)
        cc -std=c11 -O2 -DSG_MUTANT_$m -o "$out" "$HERE/sg-deskcomp.c" -lX11 -lXcomposite -lXdamage -lXfixes -lXrender -lXext -lm \
            || { echo "SKIP: cannot build the mutant"; exit 77; }
        if SG_DESKCOMP="$out" sh "$0" > "$out.log" 2>&1; then
            echo "FAIL  mutant $m passed the gate"; rc=1
        else
            echo "PASS  mutant $m fails the gate: $(grep '^FAIL' "$out.log" | head -2 | tr '\n' ' ')"
        fi
        rm -f "$out" "$out.log"
    done
    exit $rc
fi

RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
command -v "$MINGW" >/dev/null || { echo "SKIP: $MINGW not installed"; exit 77; }
for t in xvfb-run xdotool xwininfo import convert; do command -v $t >/dev/null || { echo "SKIP: needs $t"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
[ -x "$DESKCOMP" ] || { echo "SKIP: $DESKCOMP not built"; exit 77; }

T=$(mktemp -d /var/tmp/sg-deskcomp.XXXXXX)
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config"
mkdir -p "$XDG_CONFIG_HOME/stained-glass"
export WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
trap '"$WINESERVER" -k 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$T"' EXIT INT TERM
"$MINGW" -O2 -municode -mwindows -o "$T/deskcomp-probe.exe" "$HERE/test/deskcomp-probe.c" || { fail "probe did not build"; exit 1; }
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
cp "$T/deskcomp-probe.exe" "$WINEPREFIX/drive_c/"
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x700 /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Control Panel\Colors' /v Background /d '0 255 0' /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Control Panel\Desktop' /v Wallpaper /d '' /f >/dev/null 2>&1
"$WINESERVER" -w
printf 'shadows=1\nshadow=modern\nanimations=1\nopen=fade\nminimize=lamp\nwobbly=1\nmoving=0\n' > "$XDG_CONFIG_HOME/stained-glass/effects.conf"

cat > "$T/session.sh" <<EOF2
#!/bin/sh
cd "$WINEPREFIX/drive_c"
D="$T/dump"
"$WINE" explorer /desktop=shell,1024x700 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i + 1)); done
sleep 2
"$WINE" deskcomp-probe.exe win Main 100 80 400 300 &
sleep 3
"$DESKCOMP" -dump "\$D" > "$T/deskcomp.log" 2>&1 & CP=\$!
i=0; while ! grep -q 'background=1' "\$D" 2>/dev/null && [ \$i -lt 40 ]; do sleep 0.25; i=\$((i + 1)); done
"$WINE" deskcomp-probe.exe layered 600 100 128 &
"$WINE" deskcomp-probe.exe argb 600 300 &
# frosted over Main's right edge (white | green at x 500)
"$WINE" deskcomp-probe.exe frosted 400 250 &
sleep 3
import -window root "$T/on.png"
cp "\$D" "$T/dump.on"
# a new window: it fades in
"$WINE" deskcomp-probe.exe win Second 600 480 300 150 &
i=0; : > "$T/dump.open"
while [ \$i -lt 60 ]; do cat "\$D" >> "$T/dump.open" 2>/dev/null; sleep 0.05; i=\$((i + 1)); done
# a drag of Main's title bar: it wobbles, then settles where it was dropped
xdotool mousemove 250 92 mousedown 1; sleep 0.3
: > "$T/dump.drag"
for k in 1 2 3 4 5 6 7 8 9 10; do xdotool mousemove_relative -- 12 6; sleep 0.04; cat "\$D" >> "$T/dump.drag"; done
xdotool mouseup 1
sleep 2.5
cp "\$D" "$T/dump.settled"
# minimized: its picture shrinks into the taskbar
"$WINE" deskcomp-probe.exe minimize Main 2>/dev/null &
i=0; : > "$T/dump.min"
while [ \$i -lt 40 ]; do cat "\$D" >> "$T/dump.min" 2>/dev/null; sleep 0.03; i=\$((i + 1)); done
sleep 1
# a full-screen window on top: X draws it itself, the canvas hidden; gone, the canvas is back
"$WINE" deskcomp-probe.exe full & FP=\$!
sleep 3
cp "\$D" "$T/dump.full"; xwininfo -root -tree | grep '"sg-deskcomp"' | awk '{ print \$1 }' | head -1 > "$T/canvas.id"
xwininfo -id \$(cat "$T/canvas.id") 2>/dev/null | grep 'Map State' > "$T/canvas.full"
kill \$FP; sleep 3
cp "\$D" "$T/dump.back"; xwininfo -id \$(cat "$T/canvas.id") 2>/dev/null | grep 'Map State' > "$T/canvas.back"
kill \$CP; sleep 1.5
import -window root "$T/off.png"
xwininfo -root -tree > "$T/tree.off"
EOF2
chmod +x "$T/session.sh"
timeout -s KILL 240 xvfb-run -a -s "-screen 0 1024x700x24" "$T/session.sh" > "$T/session.out" 2>&1

px() { convert "$T/$1.png" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }
is() { px "$1" "$2" "$3" | awk -F, "{ r = \$1; g = \$2; b = \$3; exit !($4) }"; }
[ -f "$T/on.png" ] && [ -f "$T/dump.on" ] || { fail "no picture or dump: $(tail -3 "$T/session.out") $(cat "$T/deskcomp.log" 2>/dev/null)"; echo "RESULT: FAIL"; exit 1; }

grep -q 'background=1' "$T/dump.on" && pass "sg-deskcomp composites the desktop over its picture (_SG_DESKTOP_PIXMAP)" \
    || fail "dump: $(head -1 "$T/dump.on")"
# Main: 100,80 400x300 (with its frame); its bottom edge is near y 380
is on 300 120 'r > 200 && g > 200 && b > 200' && pass "the window is drawn ($(px on 300 120))" || fail "window: $(px on 300 120)"
is on 300 388 'g < 245' && is on 506 230 'g < 245' && is on 300 460 'g == 255 && r == 0' \
    && pass "a shadow below and beside it ($(px on 300 388), $(px on 506 230)), the desktop green farther away" \
    || fail "shadow: below $(px on 300 388) beside $(px on 506 230) far $(px on 300 460)"
is on 700 175 'r > 95 && r < 165 && g > 95 && g < 165 && b < 30' \
    && pass "a half-opaque layered window is blended over the desktop ($(px on 700 175))" || fail "layered: $(px on 700 175)"
is on 700 375 'b > 35 && b < 95 && g > 160 && g < 220 && r < 30' \
    && pass "and a window with per-pixel alpha ($(px on 700 375))" || fail "per-pixel alpha: $(px on 700 375)"
grep -E 'win 0x[0-9a-f]+ 400,250 ' "$T/dump.on" | grep -q 'acrylic=50' && is on 560 330 'r > 100 && r < 160 && g > 100 && g < 160' \
    && pass "a frosted window (_SG_ACRYLIC 50) is half see-through ($(px on 560 330))" || fail "frosted: $(grep -E ' 400,250 ' "$T/dump.on") $(px on 560 330)"
# across the edge below it: sharp, white then green, unless blurred
db=$(( $(px on 497 330 | cut -d, -f3) - $(px on 503 330 | cut -d, -f3) ))
[ "${db#-}" -lt 100 ] && pass "and what is below it is blurred (blue across the edge: $(px on 497 330) | $(px on 503 330))" \
    || fail "not blurred: $(px on 497 330) | $(px on 503 330)"

grep -E 'win 0x[0-9a-f]+ 600,480 ' "$T/dump.open" | grep -q 'anim=1' \
    && grep -E 'win 0x[0-9a-f]+ 600,480 ' "$T/dump.open" | tail -1 | grep -q 'anim=0' \
    && pass "a new window fades in, then is drawn as itself" || fail "open: $(grep -E 'win 0x[0-9a-f]+ 600,480 ' "$T/dump.open" | sort -u | head -3)"
wob=$(grep -E 'win 0x[0-9a-f]+ [0-9-]+,[0-9-]+ 400x300 .*kind=1' "$T/dump.drag" | sed -n 's/.*wobble=\([0-9.]*\).*/\1/p' | sort -n | tail -1)
awk -v m="${wob:-0}" 'BEGIN { exit !(m > 2) }' && pass "a dragged window wobbles (up to $wob px)" || fail "no wobble: ${wob:-none}"
grep -E 'win 0x[0-9a-f]+ 2[0-9][0-9],1[0-9][0-9] 400x300 .*kind=1' "$T/dump.settled" | grep -q 'wobble=0.0' \
    && pass "and settles where it was dropped ($(grep -E '400x300 .*kind=1' "$T/dump.settled" | grep -oE ' [0-9]+,[0-9]+ ' | head -1))" \
    || fail "settled: $(grep 'kind=1' "$T/dump.settled" | head -2)"
grep -q 'anim=3 ghost=1' "$T/dump.min" && pass "a minimized window's picture shrinks into the taskbar (a lamp)" \
    || fail "minimize: $(grep 'kind=1' "$T/dump.min" | sort -u | head -3)"

grep -q 'direct=0x[1-9a-f]' "$T/dump.full" && grep -q 'IsUnMapped' "$T/canvas.full" \
    && pass "a full-screen window on top is drawn by X itself, the canvas hidden ($(grep -o 'direct=0x[0-9a-f]*' "$T/dump.full"))" \
    || fail "full screen: $(grep -o 'direct=0x[0-9a-f]*' "$T/dump.full") canvas $(cat "$T/canvas.full")"
grep -q 'direct=0x0' "$T/dump.back" && grep -q 'IsViewable' "$T/canvas.back" \
    && pass "and once it is gone the canvas is back" || fail "after full screen: $(grep -o 'direct=0x[0-9a-f]*' "$T/dump.back") canvas $(cat "$T/canvas.back")"
grep -q '"sg-deskcomp"' "$T/tree.off" && fail "the canvas outlived sg-deskcomp" || pass "sg-deskcomp gone: its canvas too"
is off 300 460 'g == 255 && r == 0' && is off 700 30 'g == 255 && r == 0' \
    && pass "and X draws the desktop again ($(px off 300 460))" || fail "after: $(px off 300 460) $(px off 700 30)"

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
