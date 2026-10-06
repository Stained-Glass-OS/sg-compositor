/* unmapcfg-client: a managed X window shown and hidden again and again, as a
 * Wine message box is (Setup's "additional partitions" box), asking to be
 * moved and sized while it is hidden and as it hides -- for
 * test/unmapcfg-gate.sh. Prints "done N" after N rounds. */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	Display *d = XOpenDisplay(NULL);
	int rounds = argc > 1 ? atoi(argv[1]) : 50, i;
	if (!d) return 2;
	for (i = 0; i < rounds; i++) {
		Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 100 + i % 7, 100, 400, 200, 0, 0, 0xc0c0c0);
		XStoreName(d, w, "unmapcfg");
		XMapWindow(d, w);
		XSync(d, False);
		usleep(60000);
		XUnmapWindow(d, w);
		XMoveResizeWindow(d, w, 150 + i % 5, 120, 420 + i % 3, 220);
		XSync(d, False);
		usleep(30000);
		XMoveResizeWindow(d, w, 160, 130, 430, 230);
		XSync(d, False);
		usleep(30000);
		if (i % 2) XDestroyWindow(d, w);
		XSync(d, False);
	}
	printf("done %d\n", rounds);
	fflush(stdout);
	XCloseDisplay(d);
	return 0;
}
