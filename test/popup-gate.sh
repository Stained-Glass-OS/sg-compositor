#!/bin/sh
# Override-redirect windows (Setup's language, format and keyboard lists).
#
# Setup runs its Windows program without a Wine desktop, so a combo box's
# list is an override-redirect X window. It used to take the focus when it
# appeared -- so Wine closed it at once -- and the next click, with nothing
# focused, crashed the compositor (a NULL view in press_cursor_button).
# Here a combo box is opened and a choice made with a virtual pointer.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
WINE="${WINE:-/opt/wine-sg/bin/wine}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; T=$(mktemp -d /var/tmp/sg-popup.XXXXXX); chmod 755 "$T"; CP=""
export XDG_RUNTIME_DIR="$T/run"; mkdir -m 700 "$T/run"
# shellcheck disable=SC2317  # invoked via trap
cleanup() { [ -n "$CP" ] && kill "$CP" 2>/dev/null; WINEPREFIX="$T/prefix" "$(dirname "$WINE")/wineserver" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in "$MINGW" wayland-scanner Xwayland; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
"$MINGW" -O2 -o "$T/probe.exe" "$HERE/test/popup-probe.c" -luser32 || { echo "SKIP: cannot build the probe"; exit 77; }
wayland-scanner client-header "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/wlr-virtual-pointer-unstable-v1-client-protocol.h" &&
    wayland-scanner private-code "$HERE/test/wlr-virtual-pointer-unstable-v1.xml" "$T/vp.c" &&
    cc -O2 -I"$T" -o "$T/vptr" "$HERE/test/vptr.c" "$T/vp.c" $(pkg-config --cflags --libs wayland-client) \
    || { echo "SKIP: cannot build the virtual pointer"; exit 77; }

# a scratch Wine prefix and home, never the real ones
export HOME="$T/home" WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
mkdir -p "$HOME"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -U "$(id -u)" -- \
    sh -c "env -u WAYLAND_DISPLAY '$WINE' '$T/probe.exe' > '$T/out'; sleep 600" >"$T/log" 2>&1 &
CP=$!
wait_for() { _w=0; while ! grep -q "^$1" "$T/out" 2>/dev/null && [ $_w -lt "$2" ]; do sleep 0.5; _w=$((_w+1)); done; grep -q "^$1" "$T/out" 2>/dev/null; }
ptr() { WAYLAND_DISPLAY="$T/priv.sock" "$T/vptr" 1280 720 "$@"; }
alive() { kill -0 "$CP" 2>/dev/null; }

wait_for COMBO 240 || { fail "the probe did not start: $(cat "$T/out" 2>/dev/null)"; tail -5 "$T/log"; exit 1; }
set -- $(tr -d '\r' < "$T/out" | grep '^COMBO'); cx=$(( $2 + $4 - 8 )); cy=$(( $3 + $5 / 2 ))
ptr m "$cx" "$cy" d s 50 u
wait_for LIST 20 && pass "a click opens the list, and it stays open" || fail "the list did not stay open"
alive && pass "the compositor is still running" || fail "the compositor crashed opening the list"
set -- $(tr -d '\r' < "$T/out" | grep '^LIST' | tail -1)
[ $# -eq 5 ] && [ "$3" -ge "$cy" ] && pass "the list is below its combo box (at $2,$3)" || fail "the list is at: $*"
# the third item
[ $# -eq 5 ] && ptr m $(( $2 + 20 )) $(( $3 + $5 * 5 / 24 )) d s 50 u
wait_for SEL 20 && pass "a click on an item chooses it ($(tr -d '\r' < "$T/out" | grep '^SEL'))" || fail "no choice was made"
# nothing has the focus now if the list took it: the click that crashed
ptr m 500 300 d s 50 u; sleep 1
ptr m 500 300 d s 50 u; sleep 1
alive && pass "clicks after the list closed do not crash the compositor" || fail "the compositor crashed after the list closed"

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
