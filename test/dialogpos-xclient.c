/* dialogpos-xclient GEOMETRY: for dialogpos-gate.sh. "Main Window" asks for
 * GEOMETRY (WxH+X+Y: one display's area, so the compositor puts it on that
 * display), then "Dialog" (400x300) transient for it is shown; prints where
 * each is on the screen, as X has them:
 *   main X Y W H
 *   dialog X Y W H
 * SPDX-License-Identifier: MIT
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>

static void where(Display *d, Window w, const char *what)
{
	XWindowAttributes a;
	Window child;
	int x = 0, y = 0;

	XSync(d, False);
	XGetWindowAttributes(d, w, &a);
	XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, &x, &y, &child);
	printf("%s %d %d %d %d\n", what, x, y, a.width, a.height);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	Display *d = XOpenDisplay(NULL);
	unsigned int w = 800, h = 600;
	int x = 0, y = 0;
	Window main_w, dlg;

	if (!d || argc < 2) {
		return 1;
	}
	XParseGeometry(argv[1], &x, &y, &w, &h);
	main_w = XCreateSimpleWindow(d, DefaultRootWindow(d), x, y, w, h, 0, 0, 0x00ff00);
	XStoreName(d, main_w, "Main Window");
	XMapWindow(d, main_w);
	XSync(d, False);
	sleep(2);
	where(d, main_w, "main");
	dlg = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 400, 300, 0, 0, 0x0000ff);
	XStoreName(d, dlg, "Dialog");
	XSetTransientForHint(d, dlg, main_w);
	XMapWindow(d, dlg);
	XSync(d, False);
	sleep(2);
	where(d, dlg, "dialog");
	pause();
	return 0;
}
