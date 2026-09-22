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
2. **Lock mode with input isolation.** While locked, one lock client gets all
   input and the user's XWayland gets none. This is the load-bearing defence of
   ADR 0009 — a frozen keylogger recovers `GetAsyncKeyState` press bits on
   thaw, so the events must never arrive at all.
3. **uid-scoped privileged protocols.** Screen capture and virtual input only
   for the machine session's uid, read from `SO_PEERCRED` via
   `wl_client_get_credentials`.

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
