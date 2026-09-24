#!/bin/sh
# Secure-prompt isolation (elevation consent, ADR 0012) on the real compositor.
# SECURE must isolate exactly as LOCK does -- and tell watchers "secure", not
# "locked", so the lock service does not put up a lock screen over the prompt.
#
# A keylogger runs in the user session's XWayland. Keystrokes are injected the
# way remote access will inject them -- the virtual-keyboard protocol on the
# privileged socket -- so they travel through the compositor's own routing.
#
#   1. unlocked: a control secret must reach the keylogger (the gate has teeth)
#   2. SECURE; a watcher sees "secure"; the prompt's own X server connects
#   3. secure: keys reach the prompt AND not the keylogger
#   4. RELEASE ends it; a LOCK during a prompt is NOT undone by RELEASE
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
SG_SESSION="${SG_SESSION:-$HERE/../sg-session}"
ADV="${SG_ADVERSARY:-$SG_SESSION/build/sg-keylog-adversary.exe}"
PFX="${SG_PREFIX:-$SG_SESSION/test/tmp/state/prefix}"
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
LOCKDPY=77
RC=0
T=$(mktemp -d)
chmod 755 "$T"
CPID=""; LPID=""
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# shellcheck disable=SC2317  # invoked via trap
cleanup() {
    [ -n "$LPID" ] && kill "$LPID" 2>/dev/null
    [ -n "$CPID" ] && kill -9 "$CPID" 2>/dev/null
    WINEPREFIX="$PFX" "$WINE_DIR/bin/wineserver" -k 2>/dev/null || true
    rm -f "/tmp/.X${LOCKDPY}-lock" "/tmp/.X11-unix/X${LOCKDPY}"
    rm -rf "$T"
}
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for t in wtype xev xdotool Xwayland; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$COMP" ] && [ -f "$ADV" ] && [ -d "$PFX/drive_c" ] || { echo "SKIP: need compositor, adversary and prefix"; exit 77; }

# Windows-program logs end in \r\n; strip it or the anchor never matches.
# Count keystroke records rather than matching letters. wtype uploads its own
# keymap, so Wine's keycode heuristics decode injected text as other characters
# ("teethctl" arrives as "111232344"); a count is immune to that, and "zero new
# records while locked" catches a leak of any key at all, not just the ones a
# gate thought to look for.
keys() { tr -d '\r' < "$1" 2>/dev/null | grep -cE '^(ASYNC|LL_HOOK|KB_HOOK) '; }
ctl() { python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'$1\n'); print(s.recv(64).decode().strip())"; }
inject() { WAYLAND_DISPLAY="$T/priv.sock" wtype "$1"; }

cat > "$T/session.sh" <<EOS
#!/bin/sh
export WINEPREFIX="$PFX" WINEARCH=win64 WINEDEBUG=-all PATH="$WINE_DIR/bin:\$PATH"
echo "\$DISPLAY" > "$T/userdisplay"
wine explorer /desktop=shell,1024x768 >/dev/null 2>&1 &
sleep 8
wine "$ADV" 'Z:$(printf '%s' "$T" | tr '/' '\\\\')\\\\adv.txt' >/dev/null 2>&1 &
sleep 5
wine notepad >/dev/null 2>&1 &
wait
EOS
chmod +x "$T/session.sh"

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- "$T/session.sh" >"$T/comp.log" 2>&1 &
CPID=$!

# Wait for the keylogger to be running inside the session.
_w=0; while ! grep -q 'LL_HOOK=' "$T/adv.txt" 2>/dev/null && [ $_w -lt 90 ]; do sleep 1; _w=$((_w+1)); done
grep -q 'LL_HOOK=' "$T/adv.txt" 2>/dev/null || { fail "keylogger never started"; exit 1; }
sleep 4

# 1. Teeth.
before=$(keys "$T/adv.txt")
inject "teethctl"
_w=0; while [ "$(keys "$T/adv.txt")" -le "$before" ] && [ $_w -lt 30 ]; do sleep 0.5; _w=$((_w+1)); done
sleep 2
got=$(( $(keys "$T/adv.txt") - before ))
if [ "$got" -gt 0 ]; then pass "unlocked: injected keys reach the user session ($got records -- the gate has teeth)"
else fail "unlocked: keylogger caught nothing -- result meaningless"; echo "RESULT: FAIL"; exit 1; fi

# watcher: record every event the compositor announces
python3 -c "
import socket,sys
s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'WATCH\n')
f=open('$T/watch.txt','w')
while True:
    d=s.recv(64)
    if not d: break
    f.write(d.decode()); f.flush()
" &
WPID=$!
sleep 1

# A client that connects and hangs up without waiting for the reply must not
# take the compositor down: the reply's write used to raise SIGPIPE, so one
# connect from any local user ended the session -- or a lock screen.
python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.close()"
python3 -c "
import socket; s=socket.socket(socket.AF_UNIX); s.connect('$T/ctl.sock'); s.sendall(b'STATUS\n'); s.close()"
sleep 1
if kill -0 "$CPID" 2>/dev/null && [ "$(ctl STATUS)" = "OK unlocked" ]; then pass "a client that hangs up early does not kill the compositor"
else fail "the compositor died when a control client hung up"; echo "RESULT: FAIL"; exit 1; fi

# 2. Secure prompt, then the prompt's display connects.
[ "$(ctl SECURE)" = "OK secure" ] && pass "SECURE accepted" || fail "SECURE not accepted"
[ "$(ctl STATUS)" = "OK secure" ] && pass "STATUS reports secure" || fail "STATUS does not report secure"
sleep 1
if grep -qx secure "$T/watch.txt" && ! grep -qx locked "$T/watch.txt"; then pass "watchers told 'secure', not 'locked' (no lock screen over the prompt)"
else fail "watchers saw [$(tr '\n' ' ' < "$T/watch.txt")]"; fi
rm -f "/tmp/.X${LOCKDPY}-lock"
WAYLAND_DISPLAY="$T/priv.sock" Xwayland ":$LOCKDPY" -noreset >"$T/lockx.log" 2>&1 &
LPID=$!
sleep 3
DISPLAY=":$LOCKDPY" xev -event keyboard >"$T/xev.txt" 2>&1 &
sleep 2
_xw=$(DISPLAY=":$LOCKDPY" xdotool search --name 'Event Tester' 2>/dev/null | head -1)
[ -n "$_xw" ] && DISPLAY=":$LOCKDPY" xdotool windowfocus "$_xw" 2>/dev/null
sleep 1

# The focus-steal attack, as in lock-gate: a program in the session opens a new
# window while the prompt is up. A consent prompt a session window can take the
# keyboard from is no prompt at all -- the program could answer it itself.
UD=$(cat "$T/userdisplay")
DISPLAY="$UD" xev -event keyboard >"$T/stealer.txt" 2>&1 &
STEALER=$!
sleep 3

# 3. Isolation.
before=$(keys "$T/adv.txt")
inject "pqz4v9"
sleep 6
got=$(awk '/^KeyPress/{p=1; next} /^KeyRelease/{p=0} p && match($0, /keysym 0x[0-9a-f]+, [a-z0-9]+\)/){split(substr($0, RSTART, RLENGTH), a, " "); sub(/\)/, "", a[3]); printf "%s", a[3]; p=0}' "$T/xev.txt" 2>/dev/null)
case "$got" in
    *pqz4v9*) pass "secure: the prompt received the keys" ;;
    *) fail "secure: the prompt did not receive the keys [$got]" ;;
esac
leaked=$(( $(keys "$T/adv.txt") - before ))
if [ "$leaked" -eq 0 ]; then pass "secure: not one keystroke reached the user session's keylogger"
else fail "secure: $leaked keystroke records leaked to the user session"; fi
stolen=$(grep -c '^KeyPress' "$T/stealer.txt" 2>/dev/null); stolen=${stolen:-0}
if [ "$stolen" -eq 0 ]; then pass "secure: a window opened by the session could not steal the keyboard"
else fail "secure: a window opened during the prompt stole $stolen keystrokes"; fi
kill "$STEALER" 2>/dev/null

# 4a. RELEASE ends the prompt.
[ "$(ctl RELEASE)" = "OK unlocked" ] && pass "RELEASE ends the secure prompt" || fail "RELEASE did not return to unlocked"
sleep 1
grep -qx released "$T/watch.txt" && pass "watchers told 'released'" || fail "no 'released' event"

# 4b. A LOCK during a prompt must survive RELEASE.
ctl SECURE >/dev/null
[ "$(ctl LOCK)" = "OK locked" ] && pass "LOCK accepted during a prompt" || fail "LOCK refused during a prompt"
sleep 1
grep -qx locked "$T/watch.txt" && pass "watchers told 'locked' (lock screen goes up)" || fail "no 'locked' event"
[ "$(ctl RELEASE)" = "OK locked" ] && pass "RELEASE does not undo a real lock" || fail "RELEASE unlocked a locked machine"
[ "$(ctl SECURE)" = "ERR locked" ] && pass "no secure prompt over a locked machine" || fail "SECURE allowed while locked"
[ "$(ctl UNLOCK)" = "OK unlocked" ] && pass "UNLOCK by the lock account" || fail "UNLOCK refused"
kill "$WPID" 2>/dev/null
grep -q 'audit: secure prompt engaged' "$T/comp.log" && pass "secure prompts are audited" || fail "no audit record"

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
