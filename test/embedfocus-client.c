/* embedfocus-gate.sh's X client: a "desktop" top-level window (Wine's
 * desktop), then a program's top-level window -- which the compositor gives
 * the keyboard as it maps -- taken into the desktop as a child, as wine-sg
 * embeds a Linux program's window in its frame, and given the X focus. It
 * prints READY, then "KEY <window> <keycode>" for each key that reaches it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    Window root, desk, prog;
    XEvent e;
    if (!d) { printf("NODISPLAY\n"); return 1; }
    root = DefaultRootWindow(d);
    desk = XCreateSimpleWindow(d, root, 0, 0, 600, 400, 0, 0, 0x202020);
    XSelectInput(d, desk, KeyPressMask | FocusChangeMask);
    XMapWindow(d, desk);
    XSync(d, False);
    sleep(1);
    prog = XCreateSimpleWindow(d, root, 100, 100, 200, 100, 0, 0, 0x808080);
    XSelectInput(d, prog, KeyPressMask | FocusChangeMask | StructureNotifyMask);
    XMapWindow(d, prog);
    XSync(d, False);
    sleep(1);
    /* into the desktop window, as a child: no longer a top-level of its own */
    XReparentWindow(d, prog, desk, 20, 20);
    XMapWindow(d, prog);
    XSync(d, False);
    usleep(300000);
    XSetInputFocus(d, prog, RevertToParent, CurrentTime);
    XSync(d, False);
    printf("READY\n");
    fflush(stdout);
    for (;;) {
        XNextEvent(d, &e);
        if (e.type == KeyPress) {
            printf("KEY %s %u\n", e.xkey.window == prog ? "prog" : "desk", e.xkey.keycode);
            fflush(stdout);
        }
    }
}
