#!/bin/sh
# sg-deskcomp shows a part of the desktop's picture drawn again in place:
# wine-sg 1260 draws a rubber band dragged out on the desktop, and the icon
# under the pointer, into the picture it published (_SG_DESKTOP_PIXMAP) as
# the mouse moves -- the desktop window itself is under sg-deskcomp's canvas.
# sg-deskcomp follows the picture's damage. (David 2026-10-06: "Dragging out
# a selection box does not render, but still selects the items after you
# let go".)
#
# A stand-in desktop on Xvfb (test/bgpart-probe.c: green, a red square drawn
# into its picture after 4 s); sg-deskcomp compositing it; the screen shows
# the red square, and the rest still green and the window on it.
#
#   sh test/bgpart-gate.sh [--mutants]   (SG_DESKCOMP=<binary>, default build/sg-deskcomp)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
DESKCOMP="${SG_DESKCOMP:-$HERE/build/sg-deskcomp}"
unset DISPLAY WAYLAND_DISPLAY

if [ "${1:-}" = --mutants ]; then
    rc=0
    for m in NO_BG_DAMAGE; do
        out=$(mktemp /var/tmp/sg-bgpart-mutant.XXXXXX)
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
for t in xvfb-run import convert cc; do command -v $t >/dev/null || { echo "SKIP: needs $t"; exit 77; }; done
[ -x "$DESKCOMP" ] || { echo "SKIP: $DESKCOMP not built"; exit 77; }
T=$(mktemp -d /var/tmp/sg-bgpart.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
cc -O2 -o "$T/probe" "$HERE/test/bgpart-probe.c" -lX11 || { echo "SKIP: cannot build the probe"; exit 77; }

cat > "$T/session.sh" <<EOF2
#!/bin/sh
"$T/probe" > "$T/probe.out" & P=\$!
i=0; while [ ! -s "$T/probe.out" ] && [ \$i -lt 40 ]; do sleep 0.1; i=\$((i + 1)); done
"$DESKCOMP" -window \$(head -1 "$T/probe.out") -dump "$T/dump" > "$T/deskcomp.log" 2>&1 & C=\$!
i=0; while ! grep -q 'background=1' "$T/dump" 2>/dev/null && [ \$i -lt 30 ]; do sleep 0.1; i=\$((i + 1)); done
sleep 0.5
import -window root "$T/before.png"
sleep 4.5
import -window root "$T/after.png"
kill \$C \$P
EOF2
chmod +x "$T/session.sh"
timeout -s KILL 60 xvfb-run -a -s "-screen 0 320x240x24" "$T/session.sh" > "$T/session.out" 2>&1

px() { convert "$T/$1.png" -format "%[fx:int(255*p{$2,$3}.r)],%[fx:int(255*p{$2,$3}.g)],%[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }
[ -f "$T/after.png" ] && grep -q 'background=1' "$T/dump" 2>/dev/null || { fail "no composited desktop: $(tail -3 "$T/session.out") $(cat "$T/deskcomp.log" 2>/dev/null)"; echo "RESULT: FAIL"; exit 1; }
[ "$(px before 130 130)" = "0,192,0" ] && pass "sg-deskcomp composites the desktop's picture (green)" || fail "before: $(px before 130 130)"
[ "$(px after 130 130)" = "255,0,0" ] && pass "a square drawn into the picture in place is shown (red)" || fail "the square drawn into the picture is not shown: $(px after 130 130)"
[ "$(px after 50 50)" = "0,192,0" ] && [ "$(px after 230 40)" = "0,0,255" ] && pass "... the rest of the desktop and the window on it as they were" \
    || fail "around it: $(px after 50 50) window $(px after 230 40)"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
