/*
 * rawmotion-client -- an X client that listens for raw motion on the root
 * window, as Wine does (XI_RawMotion): Xwayland then takes the compositor's
 * relative motion too. The jump-click gate runs one beside its windows.
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>

int
main(void)
{
	Display *d = XOpenDisplay(NULL);
	unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
	XIEventMask em = {XIAllMasterDevices, sizeof(mask), mask};
	int op, ev, err, major = 2, minor = 2;
	XEvent e;

	if (!d || !XQueryExtension(d, "XInputExtension", &op, &ev, &err) || XIQueryVersion(d, &major, &minor)) {
		fprintf(stderr, "rawmotion-client: no XInput 2\n");
		return 1;
	}
	XISetMask(mask, XI_RawMotion);
	XISelectEvents(d, DefaultRootWindow(d), &em, 1);
	XFlush(d);
	for (;;) {
		XNextEvent(d, &e);
		if (e.type == GenericEvent && XGetEventData(d, &e.xcookie)) {
			XFreeEventData(d, &e.xcookie);
		}
	}
	return 0;
}
