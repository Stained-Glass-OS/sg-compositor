/*
 * frostclip-probe: a stand-in Wine desktop for test/frostclip-gate.sh.
 *
 *   frostclip-probe clipread   prints "black" when this X server reads a
 *                              clipped picture through a transform as black
 *                              (glamor), "kept" when it ignores the clip (fb)
 *   frostclip-probe desktop    a toplevel "shell - Wine Desktop" with
 *                              _SG_DESKTOP_PIXMAP, 640x480: a white window
 *                              over all of it, a black one frosted
 *                              (_SG_ACRYLIC 50) at 100,100 300x200, and a
 *                              16x16 square under the frost blinking 12 times
 *                              a second (a cursor under Start)
 *
 * SPDX-License-Identifier: MIT
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static Display *dpy;

static void transform(Picture p, double s, double dx, double dy)
{
	XTransform t = { { { XDoubleToFixed(s), 0, XDoubleToFixed(dx) },
	                   { 0, XDoubleToFixed(s), XDoubleToFixed(dy) },
	                   { 0, 0, XDoubleToFixed(1) } } };
	XRenderSetPictureTransform(dpy, p, &t);
}

static int clipread(void)
{
	/* as the frost reads: a region below the top, halved, through a clip
	 * that holds all of what is read */
	Window root = DefaultRootWindow(dpy);
	XRenderPictFormat *f = XRenderFindStandardFormat(dpy, PictStandardRGB24);
	Pixmap a = XCreatePixmap(dpy, root, 640, 480, 24), b = XCreatePixmap(dpy, root, 64, 64, 24);
	Picture pa = XRenderCreatePicture(dpy, a, f, 0, NULL), pb = XRenderCreatePicture(dpy, b, f, 0, NULL);
	XRenderColor white = { 0xffff, 0xffff, 0xffff, 0xffff };
	XRectangle r = { 100, 300, 200, 160 };
	XserverRegion reg = XFixesCreateRegion(dpy, &r, 1);
	XImage *im;
	unsigned long px;

	XRenderFillRectangle(dpy, PictOpSrc, pa, &white, 0, 0, 640, 480);
	XFixesSetPictureClipRegion(dpy, pa, 0, 0, reg);
	XRenderSetPictureFilter(dpy, pa, "bilinear", NULL, 0);
	transform(pa, 2, 100, 300);
	XRenderComposite(dpy, PictOpSrc, pa, None, pb, 0, 0, 0, 0, 0, 0, 64, 64);
	im = XGetImage(dpy, b, 0, 0, 64, 64, AllPlanes, ZPixmap);
	px = XGetPixel(im, 32, 32) & 0xffffff;
	printf("%s\n", px < 0x404040 ? "black" : "kept");
	return 0;
}

static Window child(Window parent, int x, int y, int w, int h, unsigned long bg)
{
	Window c = XCreateSimpleWindow(dpy, parent, x, y, w, h, 0, 0, bg);
	XMapWindow(dpy, c);
	return c;
}

static int desktop(void)
{
	Window root = DefaultRootWindow(dpy), d, blink;
	Pixmap bg = XCreatePixmap(dpy, root, 640, 480, DefaultDepth(dpy, DefaultScreen(dpy)));
	GC gc = XCreateGC(dpy, bg, 0, NULL);
	unsigned long acrylic = 50, pm;
	int on = 0;

	XSetForeground(dpy, gc, 0x808080);
	XFillRectangle(dpy, bg, gc, 0, 0, 640, 480);
	d = XCreateSimpleWindow(dpy, root, 0, 0, 640, 480, 0, 0, 0x808080);
	{
		/* at 0,0 with no frame: the screen's coordinates are the desktop's */
		XSetWindowAttributes swa = { .override_redirect = True };
		XChangeWindowAttributes(dpy, d, CWOverrideRedirect, &swa);
	}
	XStoreName(dpy, d, "shell - Wine Desktop");
	pm = bg;
	XChangeProperty(dpy, d, XInternAtom(dpy, "_SG_DESKTOP_PIXMAP", False), XA_PIXMAP, 32, PropModeReplace,
	                (unsigned char *)&pm, 1);
	XMapWindow(dpy, d);
	child(d, 0, 0, 640, 480, 0xffffff);
	blink = child(d, 240, 190, 16, 16, 0xffffff);
	{
		Window frost = XCreateSimpleWindow(dpy, d, 100, 100, 300, 200, 0, 0, 0x000000);
		XChangeProperty(dpy, frost, XInternAtom(dpy, "_SG_ACRYLIC", False), XA_CARDINAL, 32, PropModeReplace,
		                (unsigned char *)&acrylic, 1);
		XMapWindow(dpy, frost);
	}
	XFlush(dpy);
	for (;;) {
		usleep(80000);
		on = !on;
		XSetWindowBackground(dpy, blink, on ? 0x202020 : 0xffffff);
		XClearWindow(dpy, blink);
		XFlush(dpy);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2 || !(dpy = XOpenDisplay(NULL))) return 2;
	if (!strcmp(argv[1], "clipread")) return clipread();
	if (!strcmp(argv[1], "desktop")) return desktop();
	return 2;
}
