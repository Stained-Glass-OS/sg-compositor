# sg-compositor — the Wayland compositor

Hosts a Stained Glass desktop session. wlroots 0.18, **derived from cage
0.2.0**. See [ADR 0011](https://github.com/Stained-Glass-OS/stained-glass/blob/main/docs/decisions/0011-sg-compositor.md).

Project brief: [`stained-glass/docs/BRIEF.md`](https://github.com/Stained-Glass-OS/stained-glass/blob/main/docs/BRIEF.md).

## Build, test, gate

```sh
make build   # meson + ninja into build/
make test    # sg-session's session gate, with this compositor hosting it
make deb
```

`make test` needs `sg-session` checked out beside this repo (`SG_SESSION=`
overrides). The gate is deliberately sg-session's own: milestone 1 of ADR 0011
is "cage renamed, and the session still passes", so a new gate would prove
less than the existing one does.

## History is a diff against cage

The first commit is cage 0.2.0 verbatim, authored as its upstream. Keep it that
way: every change since is readable as a diff against upstream, which matters
when rebasing onto a newer cage or wlroots. Source file names stay cage's
(`cage.c`, `seat.c`, …) for the same reason; only the executable is renamed.

## Milestones (ADR 0011)

1. **Replace cage — done.** The session gate passes on sg-compositor exactly
   as on cage: desktop, taskbar, 32- and 64-bit Notepad.
2. **Lock mode with input isolation — done** (`lock.c`, `make test-lock`).
   While locked, only privileged clients are visible or focusable, so the
   user's XWayland is sent no input at all — the load-bearing defence of
   ADR 0009, since a frozen keylogger recovers `GetAsyncKeyState` press bits
   on thaw.
3. **Session-scoped privileged protocols — done** (`make test-privileged`).
   An ordinary client is not even offered screen capture or input injection;
   a privileged client — the machine session's account or root, by
   `SO_PEERCRED` — gets all six and can capture. Another account can neither
   connect privileged nor UNLOCK, and the refusal is audited.

## Lock mode and privileged clients (`lock.c`)

- `-L path` is the **privileged Wayland socket**: the lock screen and remote
  access connect here. `-C path` is a **control socket** taking `LOCK`,
  `UNLOCK`, `STATUS`. `-U uid` is the machine session's account.
- **Authority is `SO_PEERCRED`, never file permissions.** The compositor runs
  as the logged-in user, so that user's programs can reach any socket it
  creates. Anyone in the session may `LOCK`; only `-U`'s uid or root may
  `UNLOCK` or connect privileged. Verified across real accounts: `nobody` gets
  `ERR not permitted` and a connection reset.
- **cage offered screen capture and input injection to every client**:
  `screencopy`, `export-dmabuf`, `virtual-keyboard`, `virtual-pointer`, plus
  output and gamma control. Any program could photograph or type into a lock
  screen. A global filter now hides all six from everything but privileged
  clients; Wine uses XWayland's RandR, not these, so nothing ordinary breaks.
- Three guards do the isolation, and each is load-bearing: `desktop_view_at`
  (all pointer, touch and tablet), `seat_set_focus` (keyboard) and view
  mapping (a window opened during a lock stays hidden).
- Security events log as `audit:` at ERROR, because a release build logs
  nothing below it and a refused unlock must be visible.

**`make test-lock`** runs a keylogger in the user session and injects keys the
way remote access will — `virtual-keyboard` over the privileged socket, through
the compositor's own routing. Unlocked, the keylogger must see them (teeth);
locked, the lock screen must see them *and* the keylogger must gain zero
records; and a key-logging window the user session opens *during* the lock
must get nothing. With the focus guard removed, that window stole every key
and the lock screen got none — so the gate is known to catch it.

It counts keystroke records rather than matching letters: `wtype` uploads its
own keymap and Wine decodes injected text as other characters, and "zero new
records" catches any key leaking, not only the ones a gate thought to check.

## Things that will bite you

- **cage does not exit on SIGTERM while its child is stuck.** It stops its
  event loop and then waits for the primary client; a Wine client blocked on a
  wineserver that has already gone never returns, so neither does the
  compositor. On a real machine logind's scope kills the whole cgroup at
  logout; `sg-session`'s harness now does the same with a transient slice.
  Making the compositor itself give up on a stuck child is worth doing.
- **`-Dwerror=true` is cage's default and stays on.** Keep new code warning-free
  rather than turning it off.

## License

**MIT**, cage's licence, for the whole repository including our additions —
David's call that MIT is fine for components, and one licence avoids
mixed-licence files. cage's copyright notices stay on its files;
`debian/copyright` names both.
