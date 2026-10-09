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
   An ordinary client is not offered input injection, export-dmabuf or
   gamma, and its only screen capture is the public one (`public_capture.c`,
   since sg16) that blanks the lock and consent screens;
   a privileged client — the machine session's account or root, by
   `SO_PEERCRED` — gets all six and can capture. Another account can neither
   connect privileged nor UNLOCK, and the refusal is audited.

## Lock mode and privileged clients (`lock.c`)

- `-L path` is the **privileged Wayland socket**: the lock screen and remote
  access connect here. `-C path` is a **control socket** taking `LOCK`,
  `UNLOCK`, `STATUS`. `-U uid` is the machine session's account.
- `INHIBIT` holds the idle notifications off for as long as its connection
  stays open (sg-session's `org.freedesktop.ScreenSaver` service: Wine's power
  requests, X11 programs); anyone in the session may, like `LOCK`. `IDLE`
  says whether anything holds it. `test-power` covers both.
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

## Reserved keys (`make test-sas`)

**Win+L locks, and Ctrl+Alt+Del is the secure attention sequence.** Wine has no
SAS, and `LockWorkStation()` is a stub. The compositor sees every key before
any client, so it can reserve these: no client can grab, swallow or — since
injection is privileged — synthesise them. Both lock today.

`WATCH` on the control socket (lock account or root only) keeps the
connection open and pushes `locked` / `unlocked`, so the lock service knows to
put up the lock screen when a key locks the machine.

The gate checks that both lock, that neither the press **nor the release** of
`l`/`Delete` reaches the client, and — first — that ordinary keys do.

Two things the mutation tests taught:

- **Consuming the reserved key is defence in depth, not load-bearing.**
  `lock_engage` clears keyboard focus before the key would be delivered, so a
  build that locks without consuming still leaks nothing, and the gate rightly
  passes it. A build with no reserved-key handling at all fails on locking and
  on both leak checks.
- **A mutant must be shown to build before its verdict counts.** An early
  mutant tripped `-Werror` and never built; the gate then "failed" against a
  missing binary, which looked like a catch and was nothing of the sort.

## Remote Desktop sessions

A Remote Desktop session (sg-session's `sg-rdp-authd`, ADR 0010) is this
compositor on the **headless** backend, sized by `SG_OUTPUT_SIZE=WxH` -- the
RDP client's desktop size -- rather than wlroots' fixed 1280x720; only outputs
with no modes of their own take it. The RDP daemon is a privileged client: it
captures with screencopy and types and points with the virtual keyboard and
pointer, the same capabilities `make test-privileged` guards.

### Taking over the console session (E1b)

As on Windows, a Remote Desktop login for a user signed in at the console
takes *that* session. `REMOTE` on the control socket (lock account or root
only, because it unlocks) makes a new output on a standby headless backend,
at the console's size so the Wine desktop keeps its size. It puts every
non-privileged view on that output and privileged ones (the lock screen) on
the console. The console then shows nothing and ignores its own keyboard,
pointer and touch. Only virtual devices, the RDP daemon's, reach the session.
The reply is `OK remote <output> <W>x<H>`.

- **The RDP daemon passes its connection with `REMOTE`** (SCM_RIGHTS, one end
  of a socketpair): it becomes a privileged client, and when it closes the
  session goes back to the console, **locked**. The daemon captures the
  newest output.
- **Ctrl+Alt+Del at the console** gives the session back (locked), and the
  remote connection is destroyed at once: its capture and its input end with
  the takeover.
- `LOCAL` gives it back too. A secure prompt (SECURE) refuses `REMOTE`.
- `view_center` used to ignore the output's offset, which never mattered with
  one output.
- Gate: sg-session's `make test-rdp-stream` (console takeover section). The
  physical-input filter is not in a gate, because headless has no physical
  devices.

### Console shadow (E1 pattern A)

Remote Desktop can also *view* the console session where it is, as Windows'
shadow does: `SHADOW view` or `SHADOW control` on the control socket (lock
account or root), with one descriptor -- the RDP daemon's end of a
socketpair. Nothing moves: the console keeps its screen, its input and its
lock state; the viewer captures the console's own output. The reply is
`OK shadow <output> <W>x<H> view|control`; `SHADOWSTATUS` answers
`OK none|view|control`.

- **View only is enforced here**: a `view` connection is not offered the
  virtual keyboard and pointer globals (`global_filter`,
  `lock_restrict_input_global`), so it cannot type or point however it is
  written. Mutant `SG_MUTANT_SHADOW_VIEW_INPUT`.
- **A frame says so**: 5 px of amber (#FFB400) round the whole layout, in
  `shadow_frame_tree`, raised over everything -- a lock screen and the secure
  prompt included -- for as long as the connection lasts. The viewer sees it
  too (it is in the captured frames).
- **It ends** when the connection closes (the frame goes, nothing else
  changes -- unlike REMOTE, no lock), or on Ctrl+Alt+Del from any keyboard
  but the viewer's own virtual one (`cg_keyboard_group.owner`), which then
  goes on to the security screen as ever.
- One viewer at a time; `SHADOW` is refused (`ERR busy`) during a REMOTE
  takeover and `REMOTE` during a shadow.
- Whether another user's session may be viewed -- an administrator asking,
  the person at the console answering on the secure surface -- is settled by
  sg-rdp-authd's monitor and sg-brokerd before `SHADOW` is sent. Gate:
  sg-session's `make test-rdp-shadow`.

## Display power and idle (`make test-power`)

Settings' "Turn off the screen after" is sg-session's `sg-settingsctl`
starting `swayidle -w timeout N "wlopm --off '*'" resume "wlopm --on '*'"`
(plus `before-sleep`, which locks). Two protocols make that real:

- **ext-idle-notify-v1** (wlroots' idle notifier, cage's) -- every input
  event is activity; **idle inhibitors** (idle-inhibit-unstable-v1) hold it
  off, any inhibitor at all (cage's rule: no visibility check).
- **wlr-output-power-management-unstable-v1** (`wlopm`), new here. The XML
  is vendored in `protocol/` (not in wayland-protocols; Purism, MIT). A
  screen turned off is its output **disabled in place**: it stays in the
  layout, so no window moves and XWayland's screen keeps its size (the gate
  checks `xdpyinfo`), and nothing is rendered or captured. **Any input turns
  it back on** (`seat_notify_activity` -> `output_power_wake`), as a monitor
  wakes on Windows -- so a dead idle daemon can never leave the screen black.
  Offered to every client, not privileged: turning a screen off is what any
  Windows program may do (`SC_MONITORPOWER`). The output a Remote Desktop
  session is captured from is never turned off.

The gate (headless, 18 checks): the global offered; wlopm off/on with
capture failing while off; XWayland's size kept; a key wakes the screen;
swayidle's timeout and resume; keys resetting the timer; an inhibitor
holding a timeout off and releasing it; Settings' exact swayidle+wlopm
chain. Mutants, each shown to build: no power manager (8 fail), no wake on
input (1), input not reported as activity (2), inhibitors ignored (1).

Not yet: Windows programs cannot inhibit idle -- Wine's
`SetThreadExecutionState(ES_DISPLAY_REQUIRED)` does not reach the
compositor, and XWayland has no inhibitor of its own, so a full-screen video
in Media Player does not keep the screen on.

## Elevated programs' displays (`elevated.c`, `make test-elevated`)

**Status (2026-09-26): shipped. sg-image `make elevated-test` passes 13/13 in a VM (installer window on its own display with the real keyboard, a session program refused, Program Files + Start menu + Apps & features, Add someone else to this PC).**

ADR 0012, bug B56. An elevated program runs as SYSTEM (`sgsystem`), and on
the session's X server any session program could type into it (XTEST,
XSendEvent) or read it (XGetImage) -- the hole Windows closes with UIPI. So
each elevated program gets **an Xwayland of its own**, and this compositor is
its window manager:

- sg-session's `sg-elevated-run` (exec'd by sg-brokerd after consent, as
  SYSTEM) makes the X server's Wayland and window-manager socketpairs and a
  readiness pipe, and hands the compositor its ends with **`ELEVATED` +
  three fds (SCM_RIGHTS)** on the control socket -- the lock account or root
  only (`ERR not permitted`, audited). Then it runs `Xwayland -rootless -wm
  FD -displayfd FD -auth COOKIE` as SYSTEM and writes the display number to
  the pipe; only then does the compositor connect its window manager (xcb
  waits for the X server, which first waits for us). The X server is never
  this compositor's child: the compositor runs as the user, the one account
  that must not own an elevated display.
- wlroots' `wlr_xwayland_create_with_server()` takes a **hand-built
  `wlr_xwayland_server`** (never spawned, never restarted) and our own
  `xwayland_shell_v1`. Traps: set `xwayland->shell_v1` before emitting
  `ready` (xwm_create dereferences it); every Xwayland binds the first
  `xwayland_shell_v1` it sees and wlroots kills it for the wrong one, so the
  global filter shows each shell to its own client only -- and hides any
  shell or `wl_seat` that is being created, because it is announced to every
  client before `elevated_adopt()` has recorded it (the session's Xwayland
  bound one and died); the xwm destroys itself on hang-up but leaves
  `xwayland->xwm` dangling (detected by the seat it dropped).
- **Its windows**: a scene layer of their own, above every session window
  (a session program can neither cover nor imitate an elevated window's
  place) and below privileged views. Managed windows go where they ask
  (`request_configure`), are moved and resized by `_NET_WM_MOVERESIZE` (Wine
  sends it for a drag on its title bar; SYSTEM's HKCU has `Decorated=N`, so
  Wine draws the caption), minimised (hidden) and maximised. They keep their
  place relative to the session's layout box (Remote Desktop). While locked
  they are hidden and unfocusable like any session window.
- **Keyboard**: Alt+Tab in an elevated window gives the focus back to the
  session (on the Tab's *release*: switching on the press handed the session
  a Tab it never saw released, and it repeated for ever). `ACTIVATE <display>
  <window>` (anyone in the session: focus, not input) brings one forward --
  the taskbar's button (wine-sg 0290, through `sg-lockctl ACTIVATE`).
  `WINDOWS` lists them (`<display> <window> x y w h shown|minimized|hidden
  focused|- title`, then `END`).
- **Clipboard and drag and drop** (UIPI's rule): each elevated display has a
  hidden clipboard seat. Text the elevated program copies is offered to the
  session; the session's text is offered to it only when the user presses a
  key or a button on one of its windows (input only the compositor makes),
  never on its own; text only (proxy data sources, which also get round the
  xwm ignoring any Xwayland's selection); no primary selection, no drags
  either way, and no data device for elevated X servers. Note wlroots' xwm
  serves a paste only to a *focused* surface on either side.

**The gate** runs the elevated side as another account (`sudo -n -u
sgsystem`; 77 without it): the window appears, focused, on screen (privileged
capture), and keys reach it and not the session; a session adversary
(`test/elevated-fixture.c`) cannot connect to its X server or read its cookie,
and XTEST, XSendEvent and XGetImage reach nothing of it (teeth: the same
attack reaches a session window); Alt+Tab, ACTIVATE, click-to-focus and
drag-to-move; the clipboard rules (checked by the X selection owner, not a
paste, which the xwm refuses unfocused); a lock hides it and ACTIVATE during
a lock neither shows nor focuses it; the display ends with the program.

Mutants, each shown to build: `sg-elevated-run` keeping the requester's
DISPLAY (the program on the session's display) fails section 2 on XTEST,
XSendEvent and XGetImage; ELEVATED accepted from anyone; the session's
clipboard offered on focus rather than on input; elevated views allowed
while locked.

**The gate itself must never touch `:0`**: the agent's shell has DISPLAY=:0
(David's desktop). An adversary run without an explicit DISPLAY typed there
once. The gate unsets DISPLAY and the fixture refuses `:0` or none.

## Native Wayland windows of programs (`wayland_app.c`, `make test-wlapp`)

Linux programs are meant to be X11 clients (sg-session sets
`XDG_SESSION_TYPE=x11`, `GDK_BACKEND=x11`, ... for them) so Wine's taskbar can
frame them. One that still opens an `xdg_toplevel` -- Electron 39+ goes by
`XDG_SESSION_TYPE` alone, and **wlroots' session code sets it to `wayland`**
for everything the compositor starts -- is managed here instead of cage's
full-screen treatment: placed in the work area, clipped at the taskbar strip
(`wlr_scene_subsurface_tree_set_clip`) unless full screen, moved/resized on
its own `move`/`resize` requests, listed by `XWINDOWS` under an id with bit 31
set (X ids are 29 bits, so explorer's embedding simply fails and keeps a
hidden stand-in) and driven by `XACTIVATE`/`XMINIMIZE`/`XCLOSE`/`XKILL`.
Privileged clients (lock screen, consent prompt) keep the full-screen window.
`xdg_wm_base` is version 4 here: never send version-5 events
(`wm_capabilities`) without checking the client's version -- wlroots asserts,
and the first build took the session down the moment Claude mapped.

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

### Pens (tablet tools) and touch screens

A pen (a Surface's through iptsd, a drawing tablet) moves the cursor and
reaches programs **as a pen** through **tablet-v2** (`wlr_tablet_v2`): over a
surface that takes one (every Xwayland window, since Xwayland binds the
manager) it gets proximity, motion, pressure, tilt, rotation, distance, tip
and buttons (`tablet_tool_to_program`, with wlroots' implicit grab for a
stroke). Xwayland turns that into the X input devices `xwayland-tablet
stylus/eraser/cursor`, whose valuators carry pressure (0..65535) and tilt
(degrees) -- what GTK and Qt read, Wine's Wintab (XI1) and Wine's pen pointer
messages (wine-sg 1000, XI2) -- and which move and click the X core pointer,
so programs that know no pen still get a mouse. Over nothing that takes a pen
(the compositor's title bars, the background) the pen is the mouse as in sg37:
`process_cursor_motion`/`seat_pointer_button`, so the decor and lock guards
apply; a stroke begun there stays the mouse until the tip lifts. The barrel's
lower button is the right button, the upper the middle (Windows' mapping):
Xwayland makes BTN_STYLUS X button 2 and BTN_STYLUS2 button 3, so they are
swapped before tablet-v2. A pen tap focuses the window (`press_cursor_button`).

**Built-in screen**: touch screens, and pens not on USB/Bluetooth (or named
IPTS/Surface), are mapped to the built-in panel -- the output named eDP*/
LVDS*/DSI*, or `SG_INTERNAL_OUTPUT` -- remapped on every layout change
(`seat_map_builtin_inputs` from `handle_output_layout_change`). A udev
WL_OUTPUT still wins. Without a built-in screen, the whole layout.

Touch goes to programs as wl_touch (Xwayland: XI 2.2 touch; X emulates the
pointer for programs that do not take touches). Wine takes touches as touches
since wine-sg 1150 (WM_POINTER of PT_TOUCH, WM_TOUCH, WM_GESTURE, and the
mouse for programs that leave them to DefWindowProc).

A pen's range is the root window's `_SG_PEN_IN_RANGE` (CARDINAL 1/0, set on
tablet-tool proximity in/out, `seat_pen_range`): Xwayland's pen devices have
no proximity class, so an X program cannot otherwise tell a pen lifted out of
range from one held still -- Wine sends WM_POINTERLEAVE from it (wine-sg 1150).

**`make test-pen`** (`test/pen-gate.sh`): a test build (`-Dtest-tablet=true`,
never packaged) reads a pen's and a touch screen's events from
`SG_TEST_TABLET_FIFO` (one line per write: a real device's events come in
frames of their own, and Xwayland nets out a press and release in one frame).
Sections: xev sees the pen as a mouse; `test/penxi2-probe.c` sees the stylus
device with pressure 0.7 -> 45874 and tilt 20/-10; `test/pengtk-probe.c` (GTK
3 declared by hand: no -dev package needed) reads a pen with pressure 0.7 and
two touches; two headless outputs with `SG_INTERNAL_OUTPUT` put the pen and a
touch on the named one; `_SG_PEN_IN_RANGE` is 1 in range and 0 out; with
`SG_TEST_TABLET_LATE` the tablet comes only at a `plug` line (X gets its pen
devices then -- for wine-sg 1150's hotplug gate). Mutants: `TABLET`,
`TABLET_AS_MOUSE`, `NO_BUILTIN_MAP`, `NO_PEN_RANGE`.
