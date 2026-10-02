/* A window that draws its own title bar, as GTK 4's do (no decorations
 * wanted, _MOTIF_WM_HINTS), and asks the window manager to maximise and
 * minimise it the standard way (test/wmreq-gate.sh).
 *   wmreq-client max   -- ask maximised, then print WxH
 *   wmreq-client min   -- ask to be minimised (WM_CHANGE_STATE IconicState)
 *   wmreq-client activate SECONDS -- a green window that, after SECONDS,
 *                          asks to be brought forward (_NET_ACTIVE_WINDOW,
 *                          as SG Office does for a file opened again)
 *   wmreq-client icon  -- a window with title bar (decorations wanted) and a
 *                          16 px magenta _NET_WM_ICON (decor-gate.sh)
 *   wmreq-client desktop -- the shell's desktop stand-in (class explorer.exe,
 *                          "shell - Wine Desktop", blue), printing each key
 *                          pressed and released in it (desktopfront-gate.sh) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

static void client_message(Display *d, Window w, const char *type, long a0, long a1, long a2)
{
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = XInternAtom(d, type, False);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = a0; ev.xclient.data.l[1] = a1; ev.xclient.data.l[2] = a2;
	XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	XFlush(d);
}

int main(int argc, char **argv)
{
	Display *d = XOpenDisplay(NULL);
	Window w;
	long motif[5] = { 2, 0, 0, 0, 0 };   /* flags: decorations; decorations: none */
	XWindowAttributes a;
	if (!d || argc < 2) return 2;
	if (!strcmp(argv[1], "desktop")) {
		XClassHint ch = { "explorer.exe", "explorer.exe" };
		XEvent ev;
		w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 2560, 1600, 0, 0, 0x0000ff);
		XSetClassHint(d, w, &ch);
		XStoreName(d, w, "shell - Wine Desktop");
		XSelectInput(d, w, KeyPressMask | KeyReleaseMask);
		XMapWindow(d, w);
		XSync(d, False);
		for (;;) {
			XNextEvent(d, &ev);
			if (ev.type == KeyPress || ev.type == KeyRelease) {
				KeySym ks = XLookupKeysym(&ev.xkey, 0);
				printf("%s %s\n", ev.type == KeyPress ? "press" : "release", XKeysymToString(ks) ? XKeysymToString(ks) : "?");
				fflush(stdout);
			}
		}
	}
	w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 300, 200, 0, 0, 0x00ff00);
	XStoreName(d, w, "wmreq");
	if (!strcmp(argv[1], "icon")) {
		unsigned long icon[2 + 16 * 16];
		icon[0] = icon[1] = 16;
		for (int i = 0; i < 16 * 16; i++) icon[2 + i] = 0xffff00ff;
		XChangeProperty(d, w, XInternAtom(d, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace,
				(unsigned char *) icon, 2 + 16 * 16);
		XMapWindow(d, w);
		XSync(d, False);
		sleep(600);
		return 0;
	}
	XChangeProperty(d, w, XInternAtom(d, "_MOTIF_WM_HINTS", False), XInternAtom(d, "_MOTIF_WM_HINTS", False), 32,
			PropModeReplace, (unsigned char *) motif, 5);
	XMapWindow(d, w);
	XSync(d, False);
	sleep(2);
	if (!strcmp(argv[1], "activate") && argc > 2) {
		XEvent ev;
		sleep(atoi(argv[2]));
		client_message(d, w, "_NET_ACTIVE_WINDOW", 1 /* an application */, CurrentTime, 0);
		printf("asked\n");
		fflush(stdout);
		/* the keys this window gets (desktopfront-gate.sh: the shell's
		 * keys must go to the desktop, not here) */
		XSelectInput(d, w, KeyPressMask | KeyReleaseMask);
		for (;;) {
			XNextEvent(d, &ev);
			if (ev.type == KeyPress || ev.type == KeyRelease) {
				KeySym ks = XLookupKeysym(&ev.xkey, 0);
				printf("%s %s\n", ev.type == KeyPress ? "press" : "release", XKeysymToString(ks) ? XKeysymToString(ks) : "?");
				fflush(stdout);
			}
		}
	} else if (!strcmp(argv[1], "max")) {
		client_message(d, w, "_NET_WM_STATE", 1, XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_VERT", False),
			       XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", False));
		sleep(2);
		XGetWindowAttributes(d, w, &a);
		printf("%dx%d\n", a.width, a.height);
	} else {
		client_message(d, w, "WM_CHANGE_STATE", 3 /* IconicState */, 0, 0);
		sleep(2);
		printf("asked\n");
	}
	fflush(stdout);
	sleep(30);
	return 0;
}
