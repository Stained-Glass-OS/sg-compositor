/* sg-transient: two dialogs for xwindows-gate.sh. "Lone Dialog" is transient
 * for a window that is never mapped (Timeshift's Setup Wizard); "Owned
 * Dialog" is transient for "Main Window", which is shown. */
#include <X11/Xlib.h>
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
	main_w = mk(d, "Main Window", 0, 1);
	XSync(d, False);
	usleep(300000);
	mk(d, "Owned Dialog", main_w, 1);
	XSync(d, False);
	for (;;) {
		pause();
	}
}
