/*
 * bgpart-probe: a stand-in Wine desktop for test/bgpart-gate.sh -- a
 * toplevel "shell - Wine Desktop", 320x240, its picture (_SG_DESKTOP_PIXMAP)
 * green, a small blue window on it; after 4 seconds a red square is drawn
 * into the picture itself at 100,100 64x64 (as wine-sg 1260 draws a rubber
 * band or a lit icon into it, without a new picture), then it waits.
 *
 * SPDX-License-Identifier: MIT
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	Display *dpy = XOpenDisplay(NULL);
	Window root, d, w;
	Pixmap bg;
	GC gc;
	unsigned long pm;

	if (!dpy) return 2;
	root = DefaultRootWindow(dpy);
	bg = XCreatePixmap(dpy, root, 320, 240, DefaultDepth(dpy, DefaultScreen(dpy)));
	gc = XCreateGC(dpy, bg, 0, NULL);
	XSetForeground(dpy, gc, 0x00c000);
	XFillRectangle(dpy, bg, gc, 0, 0, 320, 240);
	d = XCreateSimpleWindow(dpy, root, 0, 0, 320, 240, 0, 0, 0x00c000);
	{
		XSetWindowAttributes swa = { .override_redirect = True };
		XChangeWindowAttributes(dpy, d, CWOverrideRedirect, &swa);
	}
	XSetWindowBackgroundPixmap(dpy, d, bg);
	XStoreName(dpy, d, "shell - Wine Desktop");
	pm = bg;
	XChangeProperty(dpy, d, XInternAtom(dpy, "_SG_DESKTOP_PIXMAP", False), XA_PIXMAP, 32, PropModeReplace,
	                (unsigned char *)&pm, 1);
	XMapWindow(dpy, d);
	w = XCreateSimpleWindow(dpy, d, 200, 20, 60, 40, 0, 0, 0x0000ff);
	XMapWindow(dpy, w);
	XSync(dpy, False);
	printf("0x%lx\n", d);
	fflush(stdout);
	sleep(4);
	XSetForeground(dpy, gc, 0xff0000);
	XFillRectangle(dpy, bg, gc, 100, 100, 64, 64);
	XSync(dpy, False);
	for (;;) pause();
	return 0;
}
