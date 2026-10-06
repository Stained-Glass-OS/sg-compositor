/* hidpi-gate's eyes: the X pointer's image over the root window (the one
 * the compositor gives Xwayland), as XFixes reports it: "WxH". */
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
int main(void)
{
    Display *d = XOpenDisplay(NULL);
    XFixesCursorImage *img;
    if (!d) return 1;
    XWarpPointer(d, None, DefaultRootWindow(d), 0, 0, 0, 0, 10, 10);
    XSync(d, False);
    if (!(img = XFixesGetCursorImage(d))) return 1;
    printf("%dx%d\n", img->width, img->height);
    return 0;
}
