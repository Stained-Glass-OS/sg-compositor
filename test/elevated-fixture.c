/*
 * Fixtures for test/elevated-gate.sh (elevated programs' displays, ADR 0012).
 *
 *   elevated-fixture target LOG
 *       The elevated program: a magenta window titled "sg-elevated-target" at
 *       200,150 (300x200) that appends "KEY <keysym>" (or "SENT <keysym>" for
 *       a synthetic event) to LOG for every key press it receives. A left
 *       button press asks the window manager to move it (_NET_WM_MOVERESIZE),
 *       as a drag on a Wine window's title bar does.
 *
 *   elevated-fixture target LOG TITLE COLOUR
 *       The same with another title and colour: the session's own window.
 *
 *   elevated-fixture keypoll LOG
 *       A session keylogger that needs no focus: polls the X server's key
 *       state (what GetAsyncKeyState is built on) and appends "DOWN <keycode>"
 *       whenever a key is down.
 *
 *   elevated-fixture owner
 *       Prints "OWNER <window>": the CLIPBOARD selection's owner (0x0: none).
 *
 *   elevated-fixture adversary ELEVATED_DISPLAY ELEVATED_WINDOW
 *       A session program attacking the elevated window, on the display it
 *       has ($DISPLAY) and the one it does not:
 *         CONNECT ok|refused   opening the elevated display without its cookie
 *         XTEST n              keys typed with XTEST on its own display
 *         XSENDEVENT n         key events sent with XSendEvent to every window
 *                              named sg-elevated-target it can see, and to the
 *                              elevated window's id
 *         XGETIMAGE n          magenta pixels it could read, from every
 *                              top-level window and the root
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

static int x_errors;
static int
on_error(Display *d, XErrorEvent *e)
{
	(void) d;
	(void) e;
	x_errors++;
	return 0;
}

static int
target(const char *log, const char *title, const char *color)
{
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "target: cannot open display\n");
		return 1;
	}
	int s = DefaultScreen(d);
	XColor magenta, exact;
	XAllocNamedColor(d, DefaultColormap(d, s), color, &magenta, &exact);
	Window w = XCreateSimpleWindow(d, RootWindow(d, s), 200, 150, 300, 200, 0, 0, magenta.pixel);
	XSizeHints hints = {.flags = USPosition | PPosition, .x = 200, .y = 150};
	XSetWMNormalHints(d, w, &hints);
	XStoreName(d, w, title);
	XWMHints wm = {.flags = InputHint, .input = True}; /* as Wine sets it */
	XSetWMHints(d, w, &wm);
	XSelectInput(d, w, KeyPressMask | ButtonPressMask | ExposureMask | FocusChangeMask);
	XMapWindow(d, w);
	XFlush(d);
	FILE *f = fopen(log, "a");
	if (!f) {
		return 1;
	}
	fprintf(f, "WINDOW %lu\n", (unsigned long) w);
	fflush(f);
	for (;;) {
		XEvent ev;
		XNextEvent(d, &ev);
		if (ev.type == KeyPress) {
			KeySym sym = XLookupKeysym(&ev.xkey, 0);
			const char *name = XKeysymToString(sym);
			fprintf(f, "%s %s\n", ev.xkey.send_event ? "SENT" : "KEY", name ? name : "?");
			fflush(f);
		} else if (ev.type == ButtonPress && ev.xbutton.button == 1) {
			/* As Wine does for a drag on its title bar: ask the window
			 * manager (the compositor) to move the window. */
			XClientMessageEvent cm = {.type = ClientMessage, .window = w, .format = 32,
						  .message_type = XInternAtom(d, "_NET_WM_MOVERESIZE", False)};
			cm.data.l[0] = ev.xbutton.x_root;
			cm.data.l[1] = ev.xbutton.y_root;
			cm.data.l[2] = 8; /* _NET_WM_MOVERESIZE_MOVE */
			cm.data.l[3] = 1;
			cm.data.l[4] = 1;
			XUngrabPointer(d, CurrentTime);
			XSendEvent(d, RootWindow(d, s), False, SubstructureRedirectMask | SubstructureNotifyMask,
				   (XEvent *) &cm);
			XFlush(d);
			fprintf(f, "BUTTON %d %d\n", ev.xbutton.x_root, ev.xbutton.y_root);
			fflush(f);
		} else if (ev.type == FocusIn) {
			fprintf(f, "FOCUS in\n");
			fflush(f);
		} else if (ev.type == FocusOut) {
			fprintf(f, "FOCUS out\n");
			fflush(f);
		}
	}
}

static int
keypoll(const char *log)
{
	Display *d = XOpenDisplay(NULL);
	FILE *f = fopen(log, "a");
	if (!d || !f) {
		return 1;
	}
	fprintf(f, "POLLING\n");
	fflush(f);
	for (;;) {
		char keys[32];
		XQueryKeymap(d, keys);
		for (int i = 0; i < 256; i++) {
			if (keys[i / 8] & (1 << (i % 8))) {
				fprintf(f, "DOWN %d\n", i);
				fflush(f);
			}
		}
		usleep(5000);
	}
}

static long
count_magenta(Display *d, Window w)
{
	XWindowAttributes a;
	long n = 0;
	if (!XGetWindowAttributes(d, w, &a) || a.map_state != IsViewable || a.width <= 0 || a.height <= 0) {
		return 0;
	}
	XImage *img = XGetImage(d, w, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
	XSync(d, False);
	if (!img) {
		return 0;
	}
	for (int y = 0; y < a.height; y += 2) {
		for (int x = 0; x < a.width; x += 2) {
			unsigned long p = XGetPixel(img, x, y) & 0xffffff;
			if (p == 0xff00ff) {
				n++;
			}
		}
	}
	XDestroyImage(img);
	return n;
}

static void
send_keys(Display *d, Window w, long *sent)
{
	for (const char *c = "sendevent"; *c; c++) {
		XKeyEvent k = {.type = KeyPress, .display = d, .window = w, .root = DefaultRootWindow(d), .subwindow = None,
			       .same_screen = True, .keycode = XKeysymToKeycode(d, XStringToKeysym((char[]){*c, 0}))};
		if (XSendEvent(d, w, True, KeyPressMask, (XEvent *) &k)) {
			(*sent)++;
		}
	}
	XSync(d, False);
}

static int
adversary(const char *elevated_display, const char *elevated_window)
{
	XSetErrorHandler(on_error);

	/* 1. The elevated display itself, with whatever credentials a session
	 *    program has (its own XAUTHORITY, none). */
	Display *e = XOpenDisplay(elevated_display);
	printf("CONNECT %s\n", e ? "ok" : "refused");
	fflush(stdout);
	if (e) {
		XCloseDisplay(e);
	}

	Display *d = XOpenDisplay(NULL);
	if (!d) {
		printf("ERROR no session display\n");
		return 1;
	}

	/* 2. XTEST: type on the session's own X server. */
	long typed = 0;
	Window focus;
	int revert;
	XGetInputFocus(d, &focus, &revert);
	printf("FOCUS 0x%lx\n", (unsigned long) focus);
	int ev, er, maj, min;
	if (XTestQueryExtension(d, &ev, &er, &maj, &min)) {
		for (const char *c = "xtestkeys"; *c; c++) {
			KeyCode kc = XKeysymToKeycode(d, XStringToKeysym((char[]){*c, 0}));
			XTestFakeKeyEvent(d, kc, True, 0);
			XSync(d, False);
			usleep(30000); /* long enough for a key-state poller to see it down */
			XTestFakeKeyEvent(d, kc, False, 0);
			XSync(d, False);
			typed++;
		}
		XSync(d, False);
	}
	printf("XTEST %ld\n", typed);
	fflush(stdout);

	/* 3. XSendEvent: to any window named like the target it can find, and
	 *    to the elevated window's id, which is meaningless here. */
	long sent = 0;
	Window root_ret, parent, *kids = NULL;
	unsigned n = 0;
	XQueryTree(d, DefaultRootWindow(d), &root_ret, &parent, &kids, &n);
	for (unsigned i = 0; i < n; i++) {
		char *name = NULL;
		if (XFetchName(d, kids[i], &name) && name && !strcmp(name, "sg-elevated-target")) {
			send_keys(d, kids[i], &sent);
		}
		if (name) {
			XFree(name);
		}
	}
	x_errors = 0;
	send_keys(d, (Window) strtoul(elevated_window, NULL, 0), &sent);
	printf("XSENDEVENT %ld (errors %d)\n", sent, x_errors);
	fflush(stdout);

	/* 4. XGetImage: every top-level window and the root. */
	long seen = count_magenta(d, DefaultRootWindow(d));
	for (unsigned i = 0; i < n; i++) {
		seen += count_magenta(d, kids[i]);
	}
	printf("XGETIMAGE %ld\n", seen);
	if (kids) {
		XFree(kids);
	}
	XCloseDisplay(d);
	return 0;
}

int
main(int argc, char **argv)
{
	/* A gate fixture types and sends events: never on a real desktop. */
	const char *dpy = getenv("DISPLAY");
	if (!dpy || !strcmp(dpy, ":0") || !strcmp(dpy, ":0.0")) {
		fprintf(stderr, "elevated-fixture: refusing DISPLAY=%s (a test display only)\n", dpy ? dpy : "(unset)");
		return 2;
	}
	if (argc == 3 && !strcmp(argv[1], "target")) {
		return target(argv[2], "sg-elevated-target", "#ff00ff");
	}
	if (argc == 5 && !strcmp(argv[1], "target")) {
		return target(argv[2], argv[3], argv[4]);
	}
	if (argc == 3 && !strcmp(argv[1], "keypoll")) {
		return keypoll(argv[2]);
	}
	if (argc == 2 && !strcmp(argv[1], "owner")) {
		/* Who owns CLIPBOARD here: what the elevated display has been offered. */
		Display *d = XOpenDisplay(NULL);
		if (!d) {
			return 1;
		}
		printf("OWNER 0x%lx\n", (unsigned long) XGetSelectionOwner(d, XInternAtom(d, "CLIPBOARD", False)));
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "adversary")) {
		return adversary(argv[2], argv[3]);
	}
	fprintf(stderr, "usage: elevated-fixture target LOG [TITLE COLOUR] | keypoll LOG | adversary DISPLAY WINDOW\n");
	return 2;
}
