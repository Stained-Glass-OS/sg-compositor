#!/bin/sh
# The session ends when its primary client exits, whatever that client left
# running (sign-out: sg-run-explorer exits, but wineserver, Wine's services
# and helpers it started live on a moment). The compositor watched a pipe
# whose write end every one of those inherited, so it never saw the exit and
# the session stayed up (2026-10-09); it watches the client's pidfd now.
#   1. the client starts a long-lived child (it inherits everything) and
#      exits: the compositor ends within 5 s, with the client's exit status
#   2. MUTANT SG_MUTANT_SESSION_END_PIPE_ONLY (the old pipe) is caught
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
COMP="${SG_COMPOSITOR_BIN:-$HERE/build/sg-compositor}"
T=$(mktemp -d); RC=0; KIDS=
cleanup() { for p in $KIDS; do kill -9 "$p" 2>/dev/null; done; [ -s "$T/orphan" ] && kill "$(cat "$T/orphan")" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
[ -x "$COMP" ] || { echo "SKIP: no compositor at $COMP"; exit 77; }

# run_session [ENV=VAL]: the client leaves a child behind and exits with 7;
# prints the compositor's exit status, or "running" after 5 s
run_session() {
    rm -f "$T/orphan" "$T/ready"
    env "$@" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
        "$COMP" -L "$T/priv.sock" -U "$(id -u)" -- \
        sh -c "sleep 120 & echo \$! > $T/orphan; sleep 2; echo up > $T/ready; exit 7" >"$T/log" 2>&1 &
    cp=$!; KIDS="$KIDS $cp"
    i=0; while [ ! -s "$T/ready" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
    i=0; while kill -0 "$cp" 2>/dev/null && [ $i -lt 25 ]; do sleep 0.2; i=$((i + 1)); done
    if kill -0 "$cp" 2>/dev/null; then echo running; kill -9 "$cp" 2>/dev/null; wait "$cp" 2>/dev/null
    else wait "$cp"; echo $?; fi
    [ -s "$T/orphan" ] && kill "$(cat "$T/orphan")" 2>/dev/null
}

r=$(run_session SG_X=1)
[ "$r" = 7 ] && pass "the session's client exits, leaving a child running: the session ends, with its status (7)" \
    || fail "after the client exited: $r (want 7); log: $(tail -3 "$T/log")"
r=$(run_session SG_MUTANT_SESSION_END_PIPE_ONLY=1)
[ "$r" = running ] && pass "MUTANT SESSION_END_PIPE_ONLY (the old pipe): the session stays up -- caught" \
    || fail "the mutant ended the session ($r): the gate cannot tell"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
