/* shelldesk-xclient: an X window like the session's desktop (Wine's
 * "shell - Wine Desktop", class explorer.exe): as big as the X screen, full
 * screen (_NET_WM_STATE_FULLSCREEN, as winex11 keeps a virtual desktop).
 * Prints "geometry X Y W H" each time it moves or changes size, and asks
 * for full screen again after each, as Wine does (test/shelldesk-gate.sh).
 * SPDX-License-Identifier: MIT */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>

static void
ask_fullscreen(Display *dpy, Window w)
{
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = 1; /* add */
	ev.xclient.data.l[1] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	ev.xclient.data.l[3] = 1;
	XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XFlush(dpy);
}

int
main(void)
{
	Display *dpy = XOpenDisplay(NULL);
	XClassHint hint = {"explorer.exe", "explorer.exe"};
	Window w;
	XEvent ev;
	int lx = -99999, ly = -99999, lw = -1, lh = -1;

	if (!dpy) {
		return 1;
	}
	w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, DisplayWidth(dpy, 0), DisplayHeight(dpy, 0), 0, 0,
				WhitePixel(dpy, 0));
	XStoreName(dpy, w, "shell - Wine Desktop");
	XSetClassHint(dpy, w, &hint);
	XSelectInput(dpy, w, StructureNotifyMask);
	XMapWindow(dpy, w);
	XSync(dpy, False);
	ask_fullscreen(dpy, w);
	for (;;) {
		XNextEvent(dpy, &ev);
		if (ev.type != ConfigureNotify && ev.type != MapNotify) {
			continue;
		}
		Window root, child;
		int x, y;
		unsigned int width, height, bw, depth;
		XGetGeometry(dpy, w, &root, &x, &y, &width, &height, &bw, &depth);
		XTranslateCoordinates(dpy, w, root, 0, 0, &x, &y, &child);
		if (x != lx || y != ly || (int)width != lw || (int)height != lh) {
			printf("geometry %d %d %u %u\n", x, y, width, height);
			fflush(stdout);
			lx = x, ly = y, lw = width, lh = height;
			ask_fullscreen(dpy, w);
		}
	}
	return 0;
}
