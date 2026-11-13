/* fullscreen-xclient: an X window that asks the window manager for full
 * screen (_NET_WM_STATE_FULLSCREEN), as Firefox and the Remote Desktop client
 * do, then prints where it was put: "geometry X Y W H" (test/onedisplay-gate.sh).
 * SPDX-License-Identifier: MIT */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(void)
{
	Display *dpy = XOpenDisplay(NULL);
	Window w, child;
	XEvent ev;
	int x, y;
	unsigned int width, height, bw, depth;
	Window root;

	if (!dpy) {
		return 1;
	}
	w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 100, 100, 400, 300, 0, 0, WhitePixel(dpy, 0));
	XStoreName(dpy, w, "fullscreen-xclient");
	XMapWindow(dpy, w);
	XSync(dpy, False);
	sleep(2);
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = 1; /* add */
	ev.xclient.data.l[1] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	ev.xclient.data.l[3] = 1;
	XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XSync(dpy, False);
	sleep(2);
	XGetGeometry(dpy, w, &root, &x, &y, &width, &height, &bw, &depth);
	XTranslateCoordinates(dpy, w, root, 0, 0, &x, &y, &child);
	printf("geometry %d %d %u %u\n", x, y, width, height);
	fflush(stdout);
	sleep(600);
	return 0;
}
