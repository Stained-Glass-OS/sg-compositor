/* A window that draws its own title bar and has the window manager move it
 * when its title is dragged, as SG Office's editors (Qt's startSystemMove)
 * and GTK 4 programs do: on a button press it sends _NET_WM_MOVERESIZE
 * (_NET_WM_MOVERESIZE_MOVE, button 1) to the root. When the button comes
 * up it resizes itself, as a program does, which must not undo the move.
 * It prints where it is whenever that changes ("at X Y") --
 * test/selfmove-gate.sh. */
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	long motif[5] = { 2, 0, 0, 0, 0 };   /* no decorations: it draws its own */
	XEvent ev;
	Window w, child;
	int x = -1, y = -1, nx, ny;

	if (!d) return 2;
	w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 400, 300, 0, 0, 0x0000ff);
	XStoreName(d, w, "selfmove");
	XChangeProperty(d, w, XInternAtom(d, "_MOTIF_WM_HINTS", False), XInternAtom(d, "_MOTIF_WM_HINTS", False), 32,
			PropModeReplace, (unsigned char *) motif, 5);
	XSelectInput(d, w, ButtonPressMask | ButtonReleaseMask | StructureNotifyMask);
	XMapWindow(d, w);
	for (;;) {
		XNextEvent(d, &ev);
		if (ev.type == ButtonPress && ev.xbutton.button == 1) {
			XEvent m;
			memset(&m, 0, sizeof(m));
			m.xclient.type = ClientMessage;
			m.xclient.window = w;
			m.xclient.message_type = XInternAtom(d, "_NET_WM_MOVERESIZE", False);
			m.xclient.format = 32;
			m.xclient.data.l[0] = ev.xbutton.x_root;
			m.xclient.data.l[1] = ev.xbutton.y_root;
			m.xclient.data.l[2] = 8;      /* _NET_WM_MOVERESIZE_MOVE */
			m.xclient.data.l[3] = 1;      /* button 1 */
			m.xclient.data.l[4] = 1;      /* an application */
			XUngrabPointer(d, CurrentTime);
			XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &m);
			XFlush(d);
			printf("asked to move\n");
			fflush(stdout);
		} else if (ev.type == ButtonRelease && ev.xbutton.button == 1) {
			XResizeWindow(d, w, 420, 300);
			XFlush(d);
			printf("resized\n");
			fflush(stdout);
		} else if (ev.type == ConfigureNotify) {
			XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, &nx, &ny, &child);
			if (nx != x || ny != y) {
				x = nx; y = ny;
				printf("at %d %d\n", x, y);
				fflush(stdout);
			}
		}
	}
}
