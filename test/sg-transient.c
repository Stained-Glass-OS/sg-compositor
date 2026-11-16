/* sg-transient: two dialogs, a menu and a popup for xwindows-gate.sh. "Lone Dialog" is transient
 * for a window that is never mapped (Timeshift's Setup Wizard); "Owned
 * Dialog" is transient for "Main Window", which is shown. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <unistd.h>

static Window mk(Display *d, const char *title, Window owner, int map)
{
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 10, 10, 200, 100, 0, 0, 0x00ff00);
	XStoreName(d, w, title);
	if (owner) {
		XSetTransientForHint(d, w, owner);
	}
	if (map) {
		XMapWindow(d, w);
	}
	return w;
}

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	Window hidden, main_w;

	if (!d) {
		return 1;
	}
	hidden = mk(d, "Hidden Main", 0, 0);
	mk(d, "Lone Dialog", hidden, 1);
	/* a program's menu while its window is framed (not mapped): Steam's View
	 * menu, a managed window of type POPUP_MENU; and an undecorated popup
	 * transient for the window (_MOTIF_WM_HINTS, no decorations) */
	{
		Window m = mk(d, "Lone Menu", hidden, 0);
		Atom t = XInternAtom(d, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
		XChangeProperty(d, m, XInternAtom(d, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
		                PropModeReplace, (unsigned char *) &t, 1);
		XMapWindow(d, m);
		Window p = mk(d, "Lone Popup", hidden, 0);
		long hints[5] = { 2, 0, 0, 0, 0 };   /* flags: decorations; decorations: none */
		Atom mh = XInternAtom(d, "_MOTIF_WM_HINTS", False);
		XChangeProperty(d, p, mh, mh, 32, PropModeReplace, (unsigned char *) hints, 5);
		XMapWindow(d, p);
	}
	main_w = mk(d, "Main Window", 0, 1);
	XSync(d, False);
	usleep(300000);
	mk(d, "Owned Dialog", main_w, 1);
	XSync(d, False);
	for (;;) {
		pause();
	}
}
