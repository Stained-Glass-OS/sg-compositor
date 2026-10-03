#!/bin/sh
# The animated background (Settings > Personalization > Background; David
# 2026-10-02: "an option for an active background instead of the single
# wallpaper image"), drawn by sg-deskcomp, off by default:
#   light  a light drifts behind the picture and shines through it
#   cells  the picture as stained-glass cells that gather round the windows
# A Wine virtual desktop on Xvfb with a striped picture and a file on the
# desktop (an icon); explorer sends the picture without the icons
# (wine-sg 0804, _SG_WALLPAPER_PIXMAP) while one is chosen:
#   1. light: frames go on (8 a second), the picture changes where the light
#      passes, the icon stays exactly as it was drawn
#   2. a full-screen window on top: no frames (X draws the window itself)
#   3. battery saver (on battery, 20 % or less): no frames
#   4. cells: the picture becomes cells; a window brings a ring of cells
#      round it; the icon stays
#   5. sg-deskcomp's own processor time with the light, for 10 s, under 10 %
#
#   WINE=/opt/wine-sg/bin/wine sh test/animbg-gate.sh [--mutants]
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${WINE:-/opt/wine-sg/bin/wine}"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
DESKCOMP="${SG_DESKCOMP:-$HERE/build/sg-deskcomp}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY

if [ "${1:-}" = --mutants ]; then
    rc=0
    for m in STATIC_BACKGROUND NO_BATTERY_SAVER; do
        out=$(mktemp /var/tmp/sg-animbg-mutant.XXXXXX)
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
for t in xvfb-run xwininfo import convert; do command -v $t >/dev/null || { echo "SKIP: needs $t"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
[ -x "$DESKCOMP" ] || { echo "SKIP: $DESKCOMP not built"; exit 77; }

T=$(mktemp -d /var/tmp/sg-animbg.XXXXXX)
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config"
mkdir -p "$XDG_CONFIG_HOME/stained-glass"
export WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
trap '"$WINESERVER" -k 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$T"' EXIT INT TERM
"$MINGW" -O2 -municode -mwindows -o "$T/deskcomp-probe.exe" "$HERE/test/deskcomp-probe.c" || { fail "probe did not build"; exit 1; }
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
cp "$T/deskcomp-probe.exe" "$WINEPREFIX/drive_c/"
# a picture of coloured stripes, and a file on the desktop
convert -size 1024x700 xc:'#3050c0' -fill '#c03040' -draw 'rectangle 0,0 340,700' -fill '#30a050' -draw 'rectangle 680,0 1024,700' \
    -fill '#e0c030' -draw 'rectangle 0,350 1024,520' BMP3:"$WINEPREFIX/drive_c/stripes.bmp"
desk=$(find "$WINEPREFIX/drive_c/users" -maxdepth 2 -name Desktop -type d | grep -v Public | head -1)
[ -n "$desk" ] || desk="$WINEPREFIX/drive_c/users/Public/Desktop"
mkdir -p "$desk"; echo hello > "$desk/Notes.txt"
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x700 /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Control Panel\Desktop' /v Wallpaper /d 'C:\stripes.bmp' /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Control Panel\Desktop' /v WallpaperStyle /d 2 /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Stained Glass\Effects' /v AnimatedBackground /t REG_DWORD /d 1 /f >/dev/null 2>&1
"$WINESERVER" -w
conf() { printf 'shadows=1\nshadow=modern\nanimations=1\nopen=none\nminimize=none\nwobbly=0\nmoving=0\nbackground=%s\n' "$1" > "$XDG_CONFIG_HOME/stained-glass/effects.conf"; }
conf light

cat > "$T/session.sh" <<EOF2
#!/bin/sh
cd "$WINEPREFIX/drive_c"
D="$T/dump"
"$WINE" explorer /desktop=shell,1024x700 > "$T/explorer.out" 2>&1 &
sleep 8
"$DESKCOMP" -dump "\$D" > "$T/deskcomp.log" 2>&1 & CP=\$!
i=0; while ! grep -q 'icons=1' "\$D" 2>/dev/null && [ \$i -lt 40 ]; do sleep 0.25; i=\$((i + 1)); done
sleep 1
cp "\$D" "$T/dump.0"; import -window root "$T/l0.png"
sleep 1.6
cp "\$D" "$T/dump.1"; import -window root "$T/l1.png"
# deskcomp's own processor time over 10 s
t0=\$(awk '{ print \$14 + \$15 }' /proc/\$CP/stat); sleep 10; t1=\$(awk '{ print \$14 + \$15 }' /proc/\$CP/stat)
echo "\$((t1 - t0))" > "$T/cpu"
# a full-screen window on top
"$WINE" deskcomp-probe.exe full & FP=\$!
sleep 3; cp "\$D" "$T/dump.f0"; sleep 2; cp "\$D" "$T/dump.f1"
kill \$FP; sleep 2
kill \$CP; sleep 1
# battery saver
SG_DESKCOMP_FAKE_BATTERY=15 "$DESKCOMP" -dump "\$D" > "$T/deskcomp2.log" 2>&1 & CP=\$!
sleep 3; cp "\$D" "$T/dump.b0"; sleep 2; cp "\$D" "$T/dump.b1"
kill \$CP; sleep 1
# cells, then a window
printf 'shadows=1\nshadow=modern\nanimations=1\nopen=none\nminimize=none\nwobbly=0\nmoving=0\nbackground=cells\n' > "$XDG_CONFIG_HOME/stained-glass/effects.conf"
"$DESKCOMP" -dump "\$D" > "$T/deskcomp3.log" 2>&1 & CP=\$!
sleep 4; cp "\$D" "$T/dump.c0"; import -window root "$T/c0.png"
"$WINE" deskcomp-probe.exe win Main 300 200 400 250 &
sleep 4; cp "\$D" "$T/dump.c1"; import -window root "$T/c1.png"
kill \$CP
EOF2
chmod +x "$T/session.sh"
timeout -s KILL 300 xvfb-run -a -s "-screen 0 1024x700x24" "$T/session.sh" > "$T/session.out" 2>&1

px() { convert "$T/$1.png" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }
fr() { sed -n 's/.*anim_frames=\([0-9]*\).*/\1/p' "$T/$1" 2>/dev/null; }
[ -f "$T/l1.png" ] && [ -f "$T/dump.1" ] || { fail "no pictures: $(tail -3 "$T/session.out") $(cat "$T/deskcomp.log" 2>/dev/null)"; echo "RESULT: FAIL"; exit 1; }
[ -n "${SHOTS:-}" ] && cp "$T/l0.png" "$SHOTS/animbg-light-0.png" && cp "$T/l1.png" "$SHOTS/animbg-light-1.png" && cp "$T/c0.png" "$SHOTS/animbg-cells.png" && cp "$T/c1.png" "$SHOTS/animbg-cells-window.png"

grep -q 'background=light' "$T/dump.0" && grep -q 'wallpaper=1 icons=1' "$T/dump.0" \
    && pass "the light: the picture without the icons and where they are (_SG_WALLPAPER_PIXMAP)" || fail "dump: $(sed -n 3p "$T/dump.0")"
f0=$(fr dump.0); f1=$(fr dump.1)
[ $((f1 - f0)) -ge 8 ] && [ $((f1 - f0)) -le 20 ] && pass "frames go on: $((f1 - f0)) in 1.6 s" || fail "frames: $f0 -> $f1"
diff=$(convert "$T/l0.png" "$T/l1.png" -compose difference -composite -colorspace gray -threshold 3% -format '%[fx:int(mean*w*h)]' info:)
[ "${diff:-0}" -gt 3000 ] && pass "the picture changes where the light passes ($diff pixels in 1.6 s)" || fail "the picture did not change ($diff pixels)"
# the icon (the top-left of the desktop): unchanged by the light
ic=$(convert "$T/l0.png" "$T/l1.png" -crop 64x48+12+12 +repage -compose difference -composite -format '%[fx:int(255*maxima)]' info: 2>/dev/null | head -1)
ink=$(convert "$T/l0.png" -crop 64x48+12+12 +repage -colorspace gray -format '%[fx:int(255*standard_deviation)]' info:)
[ "${ink:-0}" -gt 20 ] && [ "${ic:-255}" -le 2 ] && pass "the icon is drawn crisp over it, unchanged (difference $ic)" \
    || fail "icon: difference ${ic:-?}, its detail ${ink:-?}"
cpu=$(cat "$T/cpu" 2>/dev/null)
[ -n "$cpu" ] && [ "$cpu" -lt 100 ] && pass "sg-deskcomp's processor time with the light: $cpu ticks in 10 s (under 10 %)" || fail "processor time: ${cpu:-?} ticks in 10 s"
a=$(fr dump.f0); b=$(fr dump.f1)
[ -n "$a" ] && [ "$a" = "$b" ] && grep -q 'direct=0x[1-9a-f]' "$T/dump.f1" && pass "a full-screen window on top: no frames ($a, $b)" || fail "full screen: frames $a -> $b, $(head -1 "$T/dump.f1")"
a=$(fr dump.b0); b=$(fr dump.b1)
grep -q 'battery=2' "$T/dump.b1" && [ -n "$a" ] && [ "$a" = "$b" ] && pass "battery saver: no frames ($a, $b)" || fail "battery saver: frames $a -> $b, $(sed -n 3p "$T/dump.b1")"
s0=$(sed -n 's/.*sites=\([0-9]*\).*/\1/p' "$T/dump.c0"); s1=$(sed -n 's/.*sites=\([0-9]*\).*/\1/p' "$T/dump.c1")
grep -q 'background=cells' "$T/dump.c0" && [ "${s0:-0}" -gt 20 ] && [ "${s1:-0}" -gt $(( ${s0:-0} + 20 )) ] \
    && pass "cells: $s0 cells, and $s1 with a ring round the window" || fail "cells: $s0 then $s1 sites, $(sed -n 3p "$T/dump.c1")"
# lead lines: dark pixels on the desktop away from the window and icons
dark=$(convert "$T/c0.png" -crop 600x300+400+380 +repage -colorspace gray -threshold 22% -negate -format '%[fx:int(mean*w*h)]' info:)
[ "${dark:-0}" -gt 1500 ] && pass "the picture as cells with lead lines ($dark dark pixels)" || fail "no lead lines: $dark"

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
