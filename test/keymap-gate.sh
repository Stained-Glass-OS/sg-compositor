#!/bin/sh
# The session's keyboard layouts on the control socket.
#
# Settings > Time & Language > Language changes the person's keyboards
# without signing out, as on Windows (sg-session's sg-settingsctl sends
# KEYMAP), and a Windows program's ActivateKeyboardLayout of one of them
# switches the keyboard to it (wine-sg's winex11 sends LAYOUT): the keys
# type in that layout and the input indicator follows.
#
#   1. KEYBOARD: the layouts the session started with (XKB_DEFAULT_*, which
#      sg-session sets) and the one typing now; an X program sees the same
#      groups (Xwayland is sent the compositor's keymap)
#   2. LAYOUT 1: the X server's group is 1; it stays 1 after a Shift (the
#      keyboard device's own state moved too -- mutant LAYOUT_GROUP_ONLY);
#      Start key + Space (grp:win_space_toggle) still switches
#   3. KEYMAP fr,de while typing German: the X program sees French and
#      German, and German still types (mutant KEYMAP_FIRST_LAYOUT)
#   4. refusals: an unknown layout, characters XKB never names, a layout
#      past the last, LAYOUT and KEYMAP while locked
#
# A compositor built with -Dtest-tablet=true has a keyboard fed from the
# test FIFO ("kplug", "key CODE 1|0"). Headless; its own XDG_RUNTIME_DIR.
#   --mutant NAME: built with SG_MUTANT_NAME; must fail.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B=${SG_KEYMAP_BUILD:-$HERE/build-pen}
T=$(mktemp -d); RC=0; CP=
cleanup() { [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xwayland meson ninja cc python3; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
args="-Dtest-tablet=true -Dman-pages=disabled --buildtype=release"
if [ "${1:-}" = --mutant ]; then B="$B-mutant-$2"; args="$args -Dc_args=-DSG_MUTANT_$2"; fi
# shellcheck disable=SC2086  # meson's options
[ -f "$B/build.ninja" ] || meson setup "$B" "$HERE" $args >"$T/meson.log" 2>&1 || { cat "$T/meson.log"; echo "SKIP: cannot configure"; exit 77; }
ninja -C "$B" >"$T/ninja.log" 2>&1 || { cat "$T/ninja.log"; fail "the test build failed"; exit 1; }
COMP="$B/sg-compositor"
cc -O -o "$T/probe" "$HERE/test/keymap-probe.c" -lX11 || { echo "SKIP: cannot build the X probe"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY
# never the person's session: a runtime directory of our own
XDG_RUNTIME_DIR="$T/run"; mkdir -m 700 "$XDG_RUNTIME_DIR"; export XDG_RUNTIME_DIR
mkfifo "$T/in"

# the keyboard is plugged in before anything maps, so the probe's window is
# entered with it
( printf 'kplug\n' > "$T/in" ) &
XKB_DEFAULT_LAYOUT=us,de XKB_DEFAULT_VARIANT= XKB_DEFAULT_OPTIONS=grp:win_space_toggle \
    SG_TEST_TABLET_FIFO="$T/in" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "sleep 1; echo \$DISPLAY > $T/dpy; exec $T/probe hold $T/up" >"$T/log" 2>&1 &
CP=$!
_w=0; while [ ! -s "$T/up" ] && [ $_w -lt 75 ]; do sleep 0.2; _w=$((_w+1)); done
[ -s "$T/up" ] || { fail "the compositor or its X server did not start"; cat "$T/log"; exit 1; }
D=$(cat "$T/dpy")
# the compositor's own Xwayland (its child's DISPLAY), never one inherited
[ -n "$D" ] || { fail "no X display of its own"; exit 1; }
sleep 1

ctl() { python3 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(sys.argv[2].encode() + b"\n")
print(s.recv(700).decode().strip())' "$T/ctl.sock" "$1"; }
state() { DISPLAY=$D "$T/probe" state 2>&1; }
keys() { for l in "$@"; do printf '%s\n' "$l" > "$T/in"; sleep 0.15; done; sleep 0.4; }
expect() {  # expect WHAT GOT WANT
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1: got '$2', want '$3'"; fi
}

# --- 1. ---
expect "KEYBOARD: the layouts the session started with" "$(ctl KEYBOARD)" \
    "OK group=0 count=2 layout=us,de variant= options=grp:win_space_toggle"
expect "an X program sees both layouts" "$(state)" "group=0 count=2 names=English (US);German"

# --- 2. ---
expect "LAYOUT 1" "$(ctl 'LAYOUT 1')" "OK"
sleep 0.4
expect "after LAYOUT 1 the X server types German" "$(state)" "group=1 count=2 names=English (US);German"
expect "KEYBOARD says German types now" "$(ctl KEYBOARD | cut -d' ' -f2)" "group=1"
keys "key 42 1" "key 42 0"
expect "a Shift after LAYOUT 1 leaves German" "$(state)" "group=1 count=2 names=English (US);German"
keys "key 125 1" "key 57 1" "key 57 0" "key 125 0"
expect "Start key + Space still switches (back to English)" "$(state)" "group=0 count=2 names=English (US);German"

# --- 3. ---
ctl 'LAYOUT 1' >/dev/null; sleep 0.3
expect "KEYMAP fr,de" "$(ctl 'KEYMAP fr,de - grp:win_space_toggle')" "OK"
sleep 0.5
expect "the X program sees the new layouts, and German still types" "$(state)" "group=1 count=2 names=French;German"
expect "KEYBOARD: the new layouts" "$(ctl KEYBOARD)" \
    "OK group=1 count=2 layout=fr,de variant= options=grp:win_space_toggle"
expect "KEYMAP with a variant and no options" "$(ctl 'KEYMAP us,us ,dvorak -')" "OK"
sleep 0.5
expect "...English (US) and its Dvorak" "$(state | sed 's/^group=[0-9]* //')" "count=2 names=English (US);English (Dvorak)"

# --- 4. ---
expect "an unknown layout is refused" "$(ctl 'KEYMAP zz')" "ERR no such layout"
expect "characters XKB never names are refused" "$(ctl 'KEYMAP us;de')" "ERR bad keymap"
expect "five layouts are refused" "$(ctl 'KEYMAP us,de,fr,it,es')" "ERR bad keymap"
expect "a layout past the last is refused" "$(ctl 'LAYOUT 2')" "ERR no such layout"
expect "the refused keymap changed nothing" "$(state | sed 's/^group=[0-9]* //')" "count=2 names=English (US);English (Dvorak)"
ctl LOCK >/dev/null
expect "LAYOUT while locked is refused" "$(ctl 'LAYOUT 1')" "ERR locked"
expect "KEYMAP while locked is refused" "$(ctl 'KEYMAP de')" "ERR locked"
ctl UNLOCK >/dev/null
expect "after the unlock LAYOUT works again" "$(ctl 'LAYOUT 0')" "OK"

[ $RC = 0 ] || { echo "--- compositor log (tail)"; tail -20 "$T/log"; }
exit $RC
