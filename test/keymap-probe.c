/* The X server's keyboard layouts as an X program sees them (test/keymap-gate.sh).
 *
 *   keymap-probe hold FILE   map a window (so the compositor focuses it and
 *                            Xwayland is sent the keyboard's state), write
 *                            FILE once it is up, then stay
 *   keymap-probe state       print "group=N count=N names=A;B"
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "no display\n");
		return 2;
	}
	if (argc > 2 && !strcmp(argv[1], "hold")) {
		Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 400, 300, 0, 0, 0xffffff);
		XStoreName(d, w, "keymap-probe");
		XSelectInput(d, w, KeyPressMask | FocusChangeMask);
		XMapWindow(d, w);
		XSync(d, False);
		FILE *f = fopen(argv[2], "w");
		if (f) {
			fputs("up\n", f);
			fclose(f);
		}
		for (;;) {
			XEvent ev;
			XNextEvent(d, &ev);
		}
	}
	XkbStateRec st;
	int op, ev, er, ma = XkbMajorVersion, mi = XkbMinorVersion;
	XkbDescPtr desc = XkbQueryExtension(d, &op, &ev, &er, &ma, &mi) ? XkbGetMap(d, 0, XkbUseCoreKbd) : NULL;
	if (!desc || XkbGetState(d, XkbUseCoreKbd, &st) != Success) {
		fprintf(stderr, "no XKB state\n");
		return 2;
	}
	XkbGetNames(d, XkbGroupNamesMask, desc);
	XkbGetControls(d, XkbGroupsWrapMask, desc);
	int count = desc->ctrls ? desc->ctrls->num_groups : 0;
	printf("group=%d count=%d names=", st.group, count);
	for (int i = 0; i < count && i < XkbNumKbdGroups; i++) {
		char *name = desc->names && desc->names->groups[i] ? XGetAtomName(d, desc->names->groups[i]) : NULL;
		printf("%s%s", i ? ";" : "", name ? name : "");
		if (name) {
			XFree(name);
		}
	}
	printf("\n");
	XCloseDisplay(d);
	return 0;
}
