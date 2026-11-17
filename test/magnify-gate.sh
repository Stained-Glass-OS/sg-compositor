#!/bin/sh
# Magnifier, drawn by the compositor (magnifier.c). David 2026-11-17:
# "Magnifier does not seem to work on Linux apps. That needs to work just like
# the Windows one does." magnify.exe painted what Wine could paint, so X and
# Wayland programs were missing or black; now the compositor scales the frame.
#
# On a headless compositor with an X program (xterm, yellow), a native Wayland
# program (wlapp-client, green) and a Windows program (Wine, magenta) on the
# screen, the MAGNIFY connection on the control socket asks for each view and
# a capture (privileged screencopy: what is on the screen) must be, pixel for
# pixel, the unmagnified screen's region scaled (nearest, zoom 200%):
#   - full screen, round the pointer at two places (the point under the pointer
#     stays under it: source origin p - p/z, kept on the screen)
#   - lens: a rectangle round the pointer, the rest of the screen as it is
#   - docked: a band at the top showing what is round the pointer
#   - inverted colours (full screen and lens)
#   - a secure prompt and a lock are never magnified (the same picture as with
#     magnification off), and it comes back after
#   - the connection closing ends it
#   - a magnifier control (MagSetWindowSource, from another connection): its
#     client area shows its source scaled, only through the parts of it
#     nothing covers (the clip), inverted on request; it goes with its
#     connection, and the screen's view is not the control's to end
#   - coordinates given (a docked band, its centre, a full-screen origin) are
#     the Wine desktop's, wherever it sits: with the display moved to 200,100
#     in the layout, the band is still at the display's top and shows what is
#     round the centre given
# Mutants (--mutants): SG_MUTANT_MAGNIFY_WHEN_SECURE, SG_MUTANT_MAGNIFY_NO_FOLLOW
# (the origin left at 0,0), SG_MUTANT_MAGNIFY_NO_INVERT,
# SG_MUTANT_MAGNIFY_LAYOUT_COORDS (coordinates taken as the layout's),
# SG_MUTANT_MAGNIFY_NO_CLIP (a control drawn over what covers it),
# SG_MUTANT_MAGNIFY_UNDER_LINUX_WINDOWS (drawn over Linux programs' windows).
#   WINE=/path/to/wine sh test/magnify-gate.sh [--mutants]   (no WINE: no Windows window)
#   MAG_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128: on the GPU (inverting with GL)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
WINE="${WINE:-}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY

if [ "${1:-}" = --mutants ]; then
    rc=0
    for m in ${MAG_MUTANTS:-MAGNIFY_WHEN_SECURE MAGNIFY_NO_FOLLOW MAGNIFY_NO_INVERT MAGNIFY_LAYOUT_COORDS MAGNIFY_NO_CLIP MAGNIFY_UNDER_LINUX_WINDOWS}; do
        b=$(mktemp -d /var/tmp/sg-magnify-mutant.XXXXXX)
        if ! { meson setup "$b" "$HERE" -Dc_args=-DSG_MUTANT_$m >/dev/null 2>&1 && ninja -C "$b" sg-compositor >/dev/null 2>&1; }; then
            echo "SKIP: cannot build the mutant"; rm -rf "$b"; exit 77
        fi
        if SG_COMPOSITOR_BIN="$b/sg-compositor" sh "$0" > "$b/gate.log" 2>&1; then
            echo "FAIL  mutant $m passed the gate"; rc=1
        else
            echo "PASS  mutant $m fails the gate: $(grep '^FAIL' "$b/gate.log" | head -2 | tr '\n' ' ')"
        fi
        rm -rf "$b"
    done
    exit $rc
fi

T=$(mktemp -d /var/tmp/sg-magnify.XXXXXX); RC=0; CP=
cleanup() {
    exec 3>&- 2>/dev/null
    [ -s "$T/client" ] && kill "$(cat "$T/client")" 2>/dev/null
    [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null
    [ -n "$WINE" ] && WINEPREFIX="$T/prefix" "$(dirname "$WINE")/wineserver" -k 2>/dev/null
    [ -n "${KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in grim convert compare xterm Xwayland python3 wayland-scanner pkg-config cc wlr-randr; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }
XS=$(pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)/stable/xdg-shell/xdg-shell.xml
[ -r "$XS" ] || { echo "SKIP: no xdg-shell.xml"; exit 77; }
wayland-scanner client-header "$XS" "$T/xdg-shell-client-protocol.h" && wayland-scanner private-code "$XS" "$T/xdg-shell.c" &&
    cc -O2 -I"$T" -o "$T/wlapp" "$HERE/test/wlapp-client.c" "$T/xdg-shell.c" $(pkg-config --cflags --libs wayland-client) &&
    wayland-scanner client-header "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
    wayland-scanner private-code "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/vp.c" &&
    cc -O2 -I"$T" -o "$T/vptr" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client) \
    || { echo "SKIP: cannot build the test clients"; exit 77; }

# the Windows program: its own prefix, nothing of the user's
WINECMD=true
if [ -n "$WINE" ]; then
    [ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
    command -v "$MINGW" >/dev/null || { echo "SKIP: $MINGW missing"; exit 77; }
    "$MINGW" -O2 -municode -mwindows -o "$T/probe.exe" "$HERE/test/magnify-probe.c" || { fail "probe did not build"; exit 1; }
    mkdir -p "$T/home"
    export HOME="$T/home" WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
    timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
    "$(dirname "$WINE")/wineserver" -w
    WINECMD="'$WINE' '$T/probe.exe'"
fi

export XDG_RUNTIME_DIR="$T" XDG_CONFIG_HOME="$T/cfg"
SG_OUTPUT_SIZE=1024x768 WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER="${MAG_RENDERER:-pixman}" \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$\$ > $T/client; xterm -class explorer.exe -T 'shell - Wine Desktop' -geometry 20x5 -bg '#000080' -fg '#000080' -e sleep 600 & sleep 1; xterm -geometry 50x12+0+0 -bg '#ffff00' -fg '#ffff00' -e sleep 600 & sleep 1; $T/wlapp -c 00ff00 -s 700x120 > $T/app.log 2>&1 & $WINECMD > $T/wine.log 2>&1 & echo up > $T/d; exec sleep 600" \
    >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/d" ] && [ $_w -lt 50 ]; do sleep 0.2; _w=$((_w+1)); done
sleep 5; [ -n "$WINE" ] && sleep 10

# the controller: one MAGNIFY connection, its lines from fd 3
mkfifo "$T/mag.in"
python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b'MAGNIFY\n')
print(s.recv(64).decode().strip(), flush=True)
for line in sys.stdin:
    s.sendall(line.encode()); print(line.strip(), '->', s.recv(64).decode().strip(), flush=True)
" "$T/ctl.sock" < "$T/mag.in" > "$T/mag.out" 2>&1 &
exec 3>"$T/mag.in"
mag() { echo "$*" >&3; sleep 0.6; }
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n'); print(s.recv(64).decode().strip())"; }
shot() { sleep 0.8; rm -f "$T/$1.png"; WAYLAND_DISPLAY="$T/priv.sock" grim -t png "$T/$1.png" >/dev/null 2>&1
         convert "$T/$1.png" -alpha off "PNG24:$T/$1.png"; }
ptr() { WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" "$W" "$H" m "$1" "$2" >/dev/null 2>&1; sleep 0.5; }
# differing pixels between two pictures (or two regions: picture[WxH+X+Y])
ae() { compare -metric AE "$1" "$2" null: 2>&1 | cut -d' ' -f1; }
# the region of a picture, scaled 200% (nearest)
zoomed() { convert "$T/$1.png" -crop "$2x$3+$4+$5" +repage -filter point -resize 200% "PNG24:$T/$6.png"; }
crop() { convert "$T/$1.png" -crop "$2x$3+$4+$5" +repage "PNG24:$T/$6.png"; }
clamp() { [ "$1" -lt "$2" ] && echo "$2" && return; [ "$1" -gt "$3" ] && echo "$3" && return; echo "$1"; }

shot plain
[ -s "$T/plain.png" ] || { fail "no capture"; tail -5 "$T/log"; exit 1; }
set -- $(convert "$T/plain.png" -format "%w %h" info:); W=$1; H=$2
echo "      screen ${W}x$H; magnify: $(head -1 "$T/mag.out" 2>/dev/null)"
count() { convert "$T/$1.png" -format %c histogram:info: | grep -i "#$2" | awk '{s+=$1} END {print s+0}'; }
y=$(count plain FFFF00); g=$(count plain 00FF00); mg=$(count plain FF00FF)
echo "      on the screen: X $y px, Wayland $g px, Windows $mg px"
[ "$y" -gt 1000 ] && [ "$g" -gt 1000 ] && { [ -z "$WINE" ] || [ "$mg" -gt 1000 ]; } \
    && pass "an X, a Wayland${WINE:+ and a Windows} program's windows are on the screen" \
    || fail "the test windows are not all on the screen (X $y, Wayland $g, Windows $mg)"

# ---- full screen, round the pointer ----
full_at() { # X Y NAME
    ptr "$1" "$2"; shot "base_$3"; mag smooth 0; mag full 200; shot "full_$3"
    ox=$(clamp $(($1 - $1 / 2)) 0 $((W / 2))); oy=$(clamp $(($2 - $2 / 2)) 0 $((H / 2)))
    zoomed "base_$3" $((W / 2)) $((H / 2)) "$ox" "$oy" "exp_$3"
    d=$(ae "$T/full_$3.png" "$T/exp_$3.png")
    [ "$d" = 0 ] && pass "full screen at 200% round the pointer at $1,$2 is the screen's $((W / 2))x$((H / 2))+$ox+$oy scaled" \
        || fail "full screen round $1,$2: $d pixels differ from the scaled region"
    mag off
}
full_at 300 260 a
full_at $((W - 100)) $((H - 60)) b
# whose windows were in what was magnified (all three, between the two)
y=$(( $(count full_a FFFF00) + $(count full_b FFFF00) )); g=$(( $(count full_a 00FF00) + $(count full_b 00FF00) ))
mg=$(( $(count full_a FF00FF) + $(count full_b FF00FF) ))
[ "$y" -gt 1000 ] && [ "$g" -gt 1000 ] && { [ -z "$WINE" ] || [ "$mg" -gt 1000 ]; } \
    && pass "the magnified screen shows the X, Wayland${WINE:+ and Windows} windows (X $y, Wayland $g${WINE:+, Windows $mg} px)" \
    || fail "magnified windows missing (X $y, Wayland $g, Windows $mg)"

# ---- inverted ----
ptr 300 260; mag smooth 0; mag full 200; mag invert 1; shot inv
convert "$T/exp_a.png" -negate "PNG24:$T/exp_inv.png"
d=$(ae "$T/inv.png" "$T/exp_inv.png")
[ "$d" = 0 ] && pass "inverted colours: the magnified screen, negated" || fail "inverted: $d pixels differ"
mag invert 0; mag off

# ---- lens ----
PX=600 PY=400
ptr $PX $PY; shot lbase; mag lens 200 300 200; shot lens
lx=$(clamp $((PX - 150)) 0 $((W - 300))); ly=$(clamp $((PY - 100)) 0 $((H - 200)))
zoomed lbase 150 100 $((PX - 75)) $((PY - 50)) lexp; crop lens 300 200 "$lx" "$ly" lgot
d=$(ae "$T/lgot.png" "$T/lexp.png")
crop lens 400 300 0 0 lout; crop lbase 400 300 0 0 loutexp; o=$(ae "$T/lout.png" "$T/loutexp.png")
[ "$d" = 0 ] && [ "$o" = 0 ] && pass "lens: 300x200 round the pointer shows what is round it at 200%, the rest as it is" \
    || fail "lens: $d pixels differ inside, $o outside"
mag invert 1; shot lensinv
crop lensinv 300 200 "$lx" "$ly" lgoti; convert "$T/lexp.png" -negate "PNG24:$T/lexpi.png"
d=$(ae "$T/lgoti.png" "$T/lexpi.png"); crop lensinv 400 300 0 0 louti; o=$(ae "$T/louti.png" "$T/loutexp.png")
[ "$d" = 0 ] && [ "$o" = 0 ] && pass "lens inverted: only the lens" || fail "lens inverted: $d inside, $o outside"
mag invert 0; mag off

# ---- docked ----
ptr $PX $PY; shot dbase; mag dock 200 0 0 $W 200; shot dock
sx=$(clamp $((PX - W / 4)) 0 $((W - W / 2))); sy=$(clamp $((PY - 50)) 0 $((H - 100)))
zoomed dbase $((W / 2)) 100 "$sx" "$sy" dexp; crop dock "$W" 200 0 0 dgot; d=$(ae "$T/dgot.png" "$T/dexp.png")
crop dock "$W" $((H - 200)) 0 200 dbelow; crop dbase "$W" $((H - 200)) 0 200 dbelowexp; o=$(ae "$T/dbelow.png" "$T/dbelowexp.png")
[ "$d" = 0 ] && [ "$o" = 0 ] && pass "docked: the band at the top shows what is round the pointer at 200%, the rest as it is" \
    || fail "docked: $d pixels differ in the band, $o below"
mag off

# ---- never over a secure prompt or a lock; back after ----
secure_case() { # ENGAGE RELEASE NAME
    ptr 300 260; mag full 200; echo "      $1: $(ctl "$1")"; shot "${3}_on"
    mag off; shot "${3}_off"
    d=$(ae "$T/${3}_on.png" "$T/${3}_off.png")
    [ "$d" = 0 ] && pass "$3: nothing magnified while it is up" || fail "$3: magnified ($d pixels differ from the unmagnified screen)"
    mag full 200; echo "      $2: $(ctl "$2")"; shot "${3}_after"
    d=$(ae "$T/${3}_after.png" "$T/full_a.png")
    [ "$d" = 0 ] && pass "$3: magnified again after it" || fail "$3: after it, $d pixels differ from the magnified screen"
    mag off
}
mag smooth 0
secure_case SECURE RELEASE "secure prompt"
secure_case LOCK UNLOCK "lock"

# ---- a magnifier control: a window view, from a connection of its own ----
mkfifo "$T/win.in"
python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b'MAGNIFY\n')
print(s.recv(64).decode().strip(), flush=True)
for line in sys.stdin:
    s.sendall(line.encode()); print(line.strip(), '->', s.recv(64).decode().strip(), flush=True)
" "$T/ctl.sock" < "$T/win.in" > "$T/win.out" 2>&1 &
exec 4>"$T/win.in"
win() { echo "$*" >&4; sleep 0.6; }
ptr 5 5; shot wbase; mag smooth 0
# the source 300,200 150x100 shown in 40,500 300x200 (200%; no window over it there), all of it visible
win window 7 300 200 150 100 40 500 300 200 0 40 500 300 200; shot wview
zoomed wbase 150 100 300 200 wexp; crop wview 300 200 40 500 wgot; d=$(ae "$T/wgot.png" "$T/wexp.png")
crop wview 500 350 400 0 wout; crop wbase 500 350 400 0 woutexp; o=$(ae "$T/wout.png" "$T/woutexp.png")
[ "$d" = 0 ] && [ "$o" = 0 ] && pass "a magnifier control shows its source at 200% in its client area, the rest as it is" \
    || fail "magnifier control: $d pixels differ inside, $o outside"
# only its left half visible (a window over the right half): the right half is left alone
win window 7 300 200 150 100 40 500 300 200 0 40 500 150 200; shot wclip
crop wclip 150 200 40 500 wlg; crop wexp 150 200 0 0 wle; l=$(ae "$T/wlg.png" "$T/wle.png")
crop wclip 150 200 190 500 wrg; crop wbase 150 200 190 500 wre; r=$(ae "$T/wrg.png" "$T/wre.png")
[ "$l" = 0 ] && [ "$r" = 0 ] && pass "covered in part: only the uncovered part of the control is drawn" \
    || fail "clip: $l pixels differ in the visible half, $r in the covered half"
win window 7 300 200 150 100 40 500 300 200 1 40 500 300 200; shot winv
convert "$T/wexp.png" -negate "PNG24:$T/wexpi.png"; crop winv 300 200 40 500 wgi; d=$(ae "$T/wgi.png" "$T/wexpi.png")
[ "$d" = 0 ] && pass "a control's colour effect inverts it, only it" || fail "control inverted: $d pixels differ"
# nothing magnified while a secure prompt is up, controls included
echo "      SECURE: $(ctl SECURE)"; shot wsec_on; win window 7 off; shot wsec_off
d=$(ae "$T/wsec_on.png" "$T/wsec_off.png"); echo "      RELEASE: $(ctl RELEASE)"
[ "$d" = 0 ] && pass "a magnifier control draws nothing while a secure prompt is up" || fail "a control was drawn over the secure prompt ($d pixels)"
# the screen view's owner ending does not end the control; the control's connection does
# under a Linux program's window (the Wayland one, above the Wine desktop): that part is not drawn over
win window 7 300 200 150 100 600 350 300 200 0 600 350 300 200; shot wunder
crop wunder 200 40 620 365 wug; crop wbase 200 40 620 365 wue; u=$(ae "$T/wug.png" "$T/wue.png")
zoomed wbase 150 100 300 200 wexp2; crop wexp2 300 70 0 130 wle2; crop wunder 300 70 600 480 wlg2; v=$(ae "$T/wlg2.png" "$T/wle2.png")
[ "$u" = 0 ] && [ "$v" = 0 ] && pass "a control partly under a Linux program's window: that window stays on top, the rest is magnified" \
    || fail "control under a Linux window: $u pixels of that window drawn over, $v differ in the rest"
win window 7 300 200 150 100 40 500 300 200 0 40 500 300 200
mag lens 200 300 200; mag off; shot wafter
crop wafter 300 200 40 500 wag; d=$(ae "$T/wag.png" "$T/wexp.png")
[ "$d" = 0 ] && pass "the screen's view going off leaves the control's view" || fail "the control's view went with the screen's ($d)"
exec 4>&-; sleep 1; shot wgone; d=$(ae "$T/wgone.png" "$T/wbase.png")
[ "$d" = 0 ] && pass "the control's connection closing ends its view" || fail "after the control's connection closed: $d pixels differ"
mag smooth 1

# ---- coordinates are the Wine desktop's, wherever it is in the layout ----
WAYLAND_DISPLAY="$T/priv.sock" wlr-randr --output HEADLESS-1 --pos 200,100 >/dev/null 2>&1; sleep 1.5
ptr 600 600; shot mbase
mag smooth 0; mag centre 300 300; mag dock 200 0 0 $W 200; shot mdock
zoomed mbase $((W / 2)) 100 $((300 - W / 4 < 0 ? 0 : 300 - W / 4)) 250 mdexp; crop mdock "$W" 200 0 0 mdgot
d=$(ae "$T/mdgot.png" "$T/mdexp.png")
[ "$d" = 0 ] && pass "display moved to 200,100: the docked band is at its top, showing what is round the centre given" \
    || fail "display moved: the docked band differs in $d pixels (coordinates taken as the layout's?)"
mag centre pointer; mag full 200 100 50; shot morg
zoomed mbase $((W / 2)) $((H / 2)) 100 50 moexp; d=$(ae "$T/morg.png" "$T/moexp.png")
[ "$d" = 0 ] && pass "display moved: a full-screen origin given (100,50) is the desktop's" || fail "display moved: full screen from 100,50 differs in $d pixels"
mag off
WAYLAND_DISPLAY="$T/priv.sock" wlr-randr --output HEADLESS-1 --pos 0,0 >/dev/null 2>&1; sleep 1.5

# ---- the connection closing ends it ----
ptr 300 260; mag full 200; exec 3>&-; sleep 1; shot closed; shot plain2
d=$(ae "$T/closed.png" "$T/base_a.png")
[ "$d" = 0 ] && pass "the Magnifier's connection closing ends magnification" || fail "after the connection closed: $d pixels differ"

[ -n "${MAG_DEBUG:-}" ] && { cat "$T/mag.out"; grep -i magnif "$T/log" | tail -5; }
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
