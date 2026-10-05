#!/bin/sh
# sg-deskcomp's frost holds still while something under it changes, on an X
# server that honours a source picture's clip (Xwayland's glamor: Intel and
# AMD hardware). David's X1 2026-10-04: an open Start menu over Firefox went
# lighter and darker 1-2 times a second -- the frost read the composite
# buffer through the frame's damage clip, which glamor reads through a
# transform as black. Xvfb (deskcomp-gate) ignores a source's clip and never
# showed it.
#
# A headless sg-compositor (GLES2 on a render node) with Xwayland; a stand-in
# Wine desktop (test/frostclip-probe.c: a white window, a black one frosted
# at 50 over it, a square blinking under the frost); sg-deskcomp; the screen
# captured 20 times: the frost must stay half white every time.
#
#   sh test/frostclip-gate.sh [--mutants]
#   SG_DESKCOMP=<binary>, SG_COMPOSITOR_BIN=<binary>, SG_RENDER_NODE=/dev/dri/renderDN
# Exit 77 (skip) where glamor is not there (no GPU, NVIDIA's GBM, Xvfb-like
# servers): run it on the hardware it is about.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
DESKCOMP="${SG_DESKCOMP:-$HERE/build/sg-deskcomp}"
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
NODE="${SG_RENDER_NODE:-/dev/dri/renderD128}"

if [ "${1:-}" = --mutants ]; then
    rc=0
    for m in BLUR_CLIPPED NOFROST; do
        out=$(mktemp /var/tmp/sg-frostclip-mutant.XXXXXX)
        cc -std=c11 -O2 -DSG_MUTANT_$m -o "$out" "$HERE/sg-deskcomp.c" -lX11 -lXcomposite -lXdamage -lXfixes -lXrender -lXext -lm \
            || { echo "SKIP: cannot build the mutant"; exit 77; }
        SG_DESKCOMP="$out" sh "$0" > "$out.log" 2>&1; r=$?
        if [ $r = 77 ]; then echo "SKIP: $(tail -1 "$out.log")"; rm -f "$out" "$out.log"; exit 77; fi
        if [ $r = 0 ]; then echo "FAIL  mutant $m passed the gate"; rc=1
        else echo "PASS  mutant $m fails the gate: $(grep '^FAIL' "$out.log" | head -1)"; fi
        rm -f "$out" "$out.log"
    done
    exit $rc
fi

for t in grim Xwayland python3; do command -v $t >/dev/null || { echo "SKIP: needs $t"; exit 77; }; done
[ -x "$DESKCOMP" ] || { echo "SKIP: $DESKCOMP not built"; exit 77; }
[ -x "$COMP" ] || { echo "SKIP: $COMP not built"; exit 77; }
[ -e "$NODE" ] || { echo "SKIP: no render node $NODE"; exit 77; }

T=$(mktemp -d /var/tmp/sg-frostclip.XXXXXX)
CP=
cleanup() {
    touch "$T/stop"
    if [ -n "$CP" ]; then
        kill "$CP" 2>/dev/null; i=0
        while kill -0 "$CP" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.1; i=$((i+1)); done
        kill -9 "$CP" 2>/dev/null
    fi
    rm -rf "$T"
}
trap cleanup EXIT INT TERM
if [ -n "${SG_FROSTCLIP_PROBE:-}" ]; then cp "$SG_FROSTCLIP_PROBE" "$T/probe"
else cc -O2 -o "$T/probe" "$HERE/test/frostclip-probe.c" -lX11 -lXfixes -lXrender || { echo "SKIP: cannot build the probe"; exit 77; }; fi

# inside the compositor: the Xwayland display and the public Wayland socket
cat > "$T/inner.sh" <<EOF
#!/bin/sh
"$T/probe" clipread > "$T/clipread" 2>&1
linger() { i=0; while [ ! -f "$T/stop" ] && [ \$i -lt 300 ]; do sleep 0.2; i=\$((i+1)); done; kill \$(jobs -p) 2>/dev/null; exit 0; }
case "\$(cat "$T/clipread")" in black) ;; *) echo done > "$T/done"; linger ;; esac
"$T/probe" desktop & sleep 1
HOME="$T" XDG_CONFIG_HOME="$T/cfg" "$DESKCOMP" > "$T/deskcomp.log" 2>&1 & sleep 2
i=0; while [ \$i -lt 20 ]; do i=\$((i+1)); grim -t ppm -g "100,100 300x200" "$T/s\$i.ppm" 2>>"$T/grim.log"; sleep 0.1; done
echo done > "$T/done"; linger
EOF
mkdir -p "$T/cfg/stained-glass" "$T/run"
(unset DISPLAY WAYLAND_DISPLAY; export XDG_RUNTIME_DIR="$T/run"
 WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE="$NODE" \
     exec "$COMP" -L "$T/run/priv.sock" -C "$T/run/ctl.sock" -U "$(id -u)" -- sh "$T/inner.sh") > "$T/comp.log" 2>&1 &
CP=$!
w=0; while [ ! -f "$T/done" ] && [ $w -lt 150 ]; do sleep 0.2; w=$((w+1)); done
case "$(cat "$T/clipread" 2>/dev/null)" in
black) ;;
kept) echo "SKIP: this X server ignores a source picture's clip (not glamor): the flicker cannot show here"; exit 77 ;;
*) echo "SKIP: no Xwayland with glamor on $NODE ($(cat "$T/clipread" 2>/dev/null | head -1))"; exit 77 ;;
esac

RC=0
res=$(python3 - "$T" <<'EOF'
import sys, glob
lo, hi, n = 999, -1, 0
for p in sorted(glob.glob(sys.argv[1] + '/s*.ppm')):
    d = open(p, 'rb').read()
    # P6\nW H\n255\n
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    for (x, y) in ((40, 40), (260, 40), (40, 160), (260, 160)):
        o = (y * w + x) * 3
        g = (px[o] + px[o + 1] + px[o + 2]) // 3
        lo, hi = min(lo, g), max(hi, g)
    n += 1
print(n, lo, hi)
EOF
)
set -- $res
if [ "${1:-0}" -lt 15 ]; then
    echo "FAIL  only ${1:-0} captures ($(tail -2 "$T/grim.log" 2>/dev/null))"; RC=1
elif [ "$2" -ge 90 ] && [ "$3" -le 170 ]; then
    echo "PASS  the frost stays half white over a blinking square, 20 frames (grey $2..$3)"
else
    echo "FAIL  the frost leaves what is below it: grey $2..$3 over 20 frames (black when read through the damage clip)"; RC=1
fi
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
