#!/bin/sh
# The Glass look's window frames are glass (David 2026-10-02: "The glass
# theme does not have the window decorations made of glass like the bottom
# bar is"). wine-sg 0801 names where a framed window's client area is
# (_SG_FRAME: left, top, right, bottom); with the Glass look and
# Transparency effects on, Settings writes glass=N to effects.conf and
# sg-deskcomp lays the frame N% opaque over a blur of what is below, the
# client area opaque as before.
#
# A Wine virtual desktop on Xvfb (a flat green desktop), the Glass frames
# (Style\Frame 2), a window with a white client area:
#   - the window has _SG_FRAME, its client area's insets;
#   - with glass=55 its frame and title bar show the green desktop through
#     (green mixed into the frame's pale blue), its client area stays white;
#   - with glass=0 (Transparency effects off) the frame is solid again.
#
#   WINE=/opt/wine-sg/bin/wine sh test/glassframe-gate.sh [--mutants]
#   SG_DESKCOMP=<binary> to test another build (default build/sg-deskcomp).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${WINE:-/opt/wine-sg/bin/wine}"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
DESKCOMP="${SG_DESKCOMP:-$HERE/build/sg-deskcomp}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY

if [ "${1:-}" = --mutants ]; then
    rc=0
    for m in SOLID_FRAMES; do
        out=$(mktemp /var/tmp/sg-glassframe-mutant.XXXXXX)
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
for t in xvfb-run xprop xwininfo import convert; do command -v $t >/dev/null || { echo "SKIP: needs $t"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
[ -x "$DESKCOMP" ] || { echo "SKIP: $DESKCOMP not built"; exit 77; }

T=$(mktemp -d /var/tmp/sg-glassframe.XXXXXX)
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config"
mkdir -p "$XDG_CONFIG_HOME/stained-glass"
export WINEPREFIX="$T/prefix" WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
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
conf() { printf 'shadows=0\nshadow=glass\nanimations=0\nopen=none\nminimize=none\nwobbly=0\nmoving=0\nglass=%s\n' "$1" > "$XDG_CONFIG_HOME/stained-glass/effects.conf"; }
conf 55

cat > "$T/session.sh" <<EOF2
#!/bin/sh
cd "$WINEPREFIX/drive_c"
D="$T/dump"
"$WINE" explorer /desktop=shell,1024x700 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i + 1)); done
sleep 2
# the Glass frames, once the desktop is up (the look's colour scheme would
# also turn the desktop's colour blue)
"$WINE" reg add 'HKCU\\Software\\Stained Glass\\Style' /v Frame /t REG_DWORD /d 2 /f >/dev/null 2>&1
sleep 1
"$WINE" deskcomp-probe.exe win Main 100 80 400 300 &
sleep 3
"$DESKCOMP" -dump "\$D" > "$T/deskcomp.log" 2>&1 & CP=\$!
i=0; while ! grep -q 'background=1' "\$D" 2>/dev/null && [ \$i -lt 40 ]; do sleep 0.25; i=\$((i + 1)); done
sleep 2
xprop -id \$(xwininfo -root -tree | awk '\$2 == "\"Main\":" { print \$1; exit }') _SG_FRAME > "$T/frameprop" 2>&1
import -window root "$T/glass.png"
cp "\$D" "$T/dump.glass"
printf 'shadows=0\nshadow=glass\nanimations=0\nopen=none\nminimize=none\nwobbly=0\nmoving=0\nglass=0\n' > "$XDG_CONFIG_HOME/stained-glass/effects.conf"
sleep 2.5
import -window root "$T/solid.png"
kill \$CP
EOF2
chmod +x "$T/session.sh"
timeout -s KILL 200 xvfb-run -a -s "-screen 0 1024x700x24" "$T/session.sh" > "$T/session.out" 2>&1

px() { convert "$T/$1.png" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }
is() { px "$1" "$2" "$3" | awk -F, "{ r = \$1; g = \$2; b = \$3; exit !($4) }"; }
[ -f "$T/glass.png" ] && [ -f "$T/solid.png" ] || { fail "no pictures: $(tail -3 "$T/session.out") $(cat "$T/deskcomp.log" 2>/dev/null)"; echo "RESULT: FAIL"; exit 1; }
[ -n "${SHOTS:-}" ] && cp "$T/glass.png" "$SHOTS/glassframe-glass.png" && cp "$T/solid.png" "$SHOTS/glassframe-solid.png"

grep -qE '_SG_FRAME\(CARDINAL\) = [0-9]+, [1-9][0-9]*, [0-9]+, [0-9]+' "$T/frameprop" \
    && pass "the framed window names its client area ($(cat "$T/frameprop"))" || fail "_SG_FRAME: $(cat "$T/frameprop")"
grep -E 'win 0x[0-9a-f]+ 100,80 ' "$T/dump.glass" | grep -q 'glass=55' \
    && pass "sg-deskcomp draws it as glass (glass=55)" || fail "dump: $(grep -E ' 100,80 ' "$T/dump.glass")"
# the left frame (x 102) and the title bar (y 92, left of the buttons): the
# Glass frame's pale blue-grey (about 167,195,230) mixed with the green desktop
is glass 102 250 'g > r + 60 && g > b + 20' && is glass 250 92 'g > r + 50 && g > b + 15' \
    && pass "the frame and title bar show the desktop through ($(px glass 102 250), $(px glass 250 92))" \
    || fail "frame not glass: $(px glass 102 250) title $(px glass 250 92)"
is glass 300 250 'r > 240 && g > 240 && b > 240' && pass "the client area stays opaque ($(px glass 300 250))" \
    || fail "client area: $(px glass 300 250)"
is solid 102 250 'b > g' && is solid 250 92 'b > g' \
    && pass "glass=0 (Transparency effects off): the frame is solid ($(px solid 102 250), $(px solid 250 92))" \
    || fail "solid frame: $(px solid 102 250) title $(px solid 250 92)"

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
