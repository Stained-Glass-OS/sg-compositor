/* xfsinit-client: an X window full screen from its start (_NET_WM_STATE set before it maps, as SDL
 * does) at X,100 (test/xfsmap-gate.sh), staying up LINGER seconds after;
 * prints where it was put: "geometry X Y W H"
 * SPDX-License-Identifier: MIT */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    Window w, child, root; int x, y; unsigned int ww, hh, bw, depth;
    int px = argc > 1 ? atoi(argv[1]) : 100;
    Atom fs;
    if (!d) return 1;
    w = XCreateSimpleWindow(d, DefaultRootWindow(d), px, 100, 400, 300, 0, 0, WhitePixel(d, 0));
    XStoreName(d, w, "xfsinit-client");
    fs = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(d, w, XInternAtom(d, "_NET_WM_STATE", False), XA_ATOM, 32, PropModeReplace, (unsigned char *)&fs, 1);
    XMapWindow(d, w); XSync(d, False);
    sleep(2);
    XGetGeometry(d, w, &root, &x, &y, &ww, &hh, &bw, &depth);
    XTranslateCoordinates(d, w, root, 0, 0, &x, &y, &child);
    printf("geometry %d %d %u %u\n", x, y, ww, hh); fflush(stdout);
    sleep(argc > 2 ? atoi(argv[2]) : 1);
    return 0;
}
