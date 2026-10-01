/*
 * sg-deskcomp -- the Wine desktop's compositing manager.
 *
 * Every Windows program's window is an X child of the one "shell - Wine
 * Desktop" toplevel (Wine's virtual desktop); X never blends children, and
 * the Wayland compositor sees only that toplevel. So no window could be
 * translucent over another, and none had a shadow (David: "The apps need to
 * have drop shadows like in windows. We also should support alpha in
 * windows.").
 *
 * sg-deskcomp redirects the desktop's children (Composite, manually: X no
 * longer draws them) and paints them itself into a canvas -- a child of the
 * desktop that takes no input -- bottom to top over the desktop's picture,
 * which wine-sg 0744 names on the desktop window (_SG_DESKTOP_PIXMAP):
 *
 *   - alpha: a window with an alpha channel (32-bit visual: layered and
 *     glass windows) is blended over what is below it, other programs'
 *     windows and the desktop included; _NET_WM_WINDOW_OPACITY too;
 *   - drop shadows, as wine-sg 0744's _SG_SHADOW asks (windows with a title
 *     bar; menus, tooltips), in the look's manner: soft and wide (Classic,
 *     Rounded), small and offset (Horizon), large and soft (Glass);
 *   - effects, each off unless chosen in Settings > Personalization >
 *     Visual effects: windows fading or zooming as they open and close,
 *     shrinking into the taskbar as they minimize (a lamp or a plain
 *     scale), wobbling while dragged, translucent while moved.
 *
 * Input is untouched: the windows stay where they are and get their clicks;
 * only their pictures are drawn by us. If sg-deskcomp exits or dies, the X
 * server ends its redirections and destroys its canvas, and the desktop is
 * drawn by X as before.
 *
 * Settings: $XDG_CONFIG_HOME/stained-glass/effects.conf (Settings writes
 * it), "key=value" lines, read again when it changes:
 *   shadows=1|0      shadow=modern|horizon|glass
 *   animations=1|0   (0: "Show animations" off -- no effect moves at all)
 *   open=none|fade|zoom   minimize=none|scale|lamp   wobbly=0|1   moving=0|1
 *
 *   sg-deskcomp [-display DPY] [-window XID] [-dump FILE]
 *
 * -dump writes what it composites after every frame (the gates read it).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum kind { KIND_PLAIN, KIND_FRAMED, KIND_POPUP };
enum anim { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE, ANIM_MINIMIZE, ANIM_RESTORE };

#define GRID 4                  /* the wobble's control points per side */
#define OFFSCREEN (-30000)      /* where Wine puts a minimized window */

struct win {
	Window id;
	int x, y, w, h, bw;
	int mapped, argb, managed;
	enum kind kind;
	unsigned long opacity;      /* _NET_WM_WINDOW_OPACITY, 0..0xffffffff */
	Damage damage;
	Pixmap pixmap;
	Picture pict;
	Picture shadow;             /* the shadow's alpha (A8), for the size below */
	int shadow_w, shadow_h, shadow_kind, shadow_style;
	XserverRegion shape;        /* its bounding shape, window coordinates; None: a rectangle */
	enum anim anim;
	double anim_start;
	int ghost;                  /* a picture kept for a closing or minimizing animation */
	double last_move;           /* when it last moved (translucent while moving, wobble grabs) */
	int wobbling, anchor;
	float ox[GRID * GRID], oy[GRID * GRID], vx[GRID * GRID], vy[GRID * GRID];
};

static Display *dpy;
static Window desktop, canvas;
static int dw, dh;
static Visual *visual;
static int depth;
static XRenderPictFormat *format, *argb_format, *a8_format;
static Picture canvas_pict, buffer_pict, bg_pict, black_pict;
static Pixmap buffer_pixmap;
static XserverRegion damage_all;
static int damage_event, damage_error, xfixes_event, xfixes_error, shape_event, shape_error;
static struct win *wins;
static int nwins, capwins;
static Atom atom_shadow, atom_opacity, atom_bgpixmap;
static const char *dump_path;
static volatile sig_atomic_t quit;
static int trapped;
static unsigned long frames;
static double last_frame;

/* the settings */
static int opt_shadows = 1, opt_anim = 1, opt_wobbly = 0, opt_moving = 0;
static char opt_shadow[16] = "modern", opt_open[16] = "none", opt_minimize[16] = "none";
static char conf_path[512];
static time_t conf_mtime;
static off_t conf_size = -1;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int error_handler(Display *d, XErrorEvent *e)
{
	(void)d; (void)e;
	trapped = 1;   /* a window gone between an event and our request: harmless */
	return 0;
}

static void damage_rect(int x, int y, int w, int h);

static void read_settings(void)
{
	struct stat st;
	FILE *f;
	char line[256];

	if (stat(conf_path, &st)) { conf_mtime = 0; conf_size = -1; return; }
	if (st.st_mtime == conf_mtime && st.st_size == conf_size) return;
	conf_mtime = st.st_mtime;
	conf_size = st.st_size;
	if (!(f = fopen(conf_path, "r"))) return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *v;
		if (!eq) continue;
		*eq = 0;
		v = eq + 1;
		v[strcspn(v, "\r\n \t")] = 0;
		if (!strcmp(line, "shadows")) opt_shadows = atoi(v) != 0;
		else if (!strcmp(line, "animations")) opt_anim = atoi(v) != 0;
		else if (!strcmp(line, "wobbly")) opt_wobbly = atoi(v) != 0;
		else if (!strcmp(line, "moving")) opt_moving = atoi(v) != 0;
		else if (!strcmp(line, "shadow")) snprintf(opt_shadow, sizeof(opt_shadow), "%s", v);
		else if (!strcmp(line, "open")) snprintf(opt_open, sizeof(opt_open), "%s", v);
		else if (!strcmp(line, "minimize")) snprintf(opt_minimize, sizeof(opt_minimize), "%s", v);
	}
	fclose(f);
	if (desktop) damage_rect(0, 0, dw, dh);   /* shadows on or off, another look: everything again */
}

static int want_open(void) { return opt_anim && strcmp(opt_open, "none"); }
static int want_minimize(void) { return opt_anim && strcmp(opt_minimize, "none"); }
#ifdef SG_MUTANT_NOEFFECTS
static int want_wobbly(void) { return 0; }
#define want_open() 0
#define want_minimize() 0
#else
static int want_wobbly(void) { return opt_anim && opt_wobbly; }
#endif

/* ---- the windows ---------------------------------------------------------- */

static struct win *find(Window id)
{
	for (int i = 0; i < nwins; i++)
		if (wins[i].id == id && !wins[i].ghost) return &wins[i];
	return NULL;
}

static struct win *new_entry(void)
{
	if (nwins == capwins) {
		capwins = capwins ? 2 * capwins : 64;
		wins = realloc(wins, capwins * sizeof(*wins));
	}
	memset(&wins[nwins], 0, sizeof(*wins));
	return &wins[nwins++];
}

static unsigned long prop_card(Window w, Atom prop, Atom type, unsigned long def)
{
	Atom actual;
	int fmt;
	unsigned long n, left, v = def;
	unsigned char *data = NULL;
	if (XGetWindowProperty(dpy, w, prop, 0, 1, False, type, &actual, &fmt, &n, &left, &data) == Success &&
	    data && n == 1 && fmt == 32)
		v = *(unsigned long *)data & 0xffffffffUL;   /* Xlib hands 32-bit items over in longs, sign-extended */
	if (data) XFree(data);
	return v;
}

/* the shadow a window has: wine-sg 0744 says, from its styles */
static enum kind window_kind(Window w)
{
	unsigned long shadow = prop_card(w, atom_shadow, XA_CARDINAL, 0);
	return shadow == 1 ? KIND_FRAMED : shadow == 2 ? KIND_POPUP : KIND_PLAIN;
}

static void free_pictures(struct win *w)
{
	if (w->pict) XRenderFreePicture(dpy, w->pict);
	if (w->pixmap) XFreePixmap(dpy, w->pixmap);
	w->pict = 0;
	w->pixmap = 0;
}

static void get_shape(struct win *w)
{
	int bounding, xb, yb, clip, xc, yc;
	unsigned int wb, hb, wc, hc;
	if (w->shape) XFixesDestroyRegion(dpy, w->shape);
	w->shape = None;
	if (XShapeQueryExtents(dpy, w->id, &bounding, &xb, &yb, &wb, &hb, &clip, &xc, &yc, &wc, &hc) && bounding)
		w->shape = XFixesCreateRegionFromWindow(dpy, w->id, WindowRegionBounding);
}

static void get_pictures(struct win *w)
{
	XWindowAttributes a;
	XRenderPictureAttributes pa = { .subwindow_mode = IncludeInferiors };
	XRenderPictFormat *f;

	free_pictures(w);
	if (!w->mapped || !w->managed || !XGetWindowAttributes(dpy, w->id, &a)) return;
	trapped = 0;
	w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
	XSync(dpy, False);
	if (trapped) { w->pixmap = 0; return; }
	f = XRenderFindVisualFormat(dpy, a.visual);
	w->argb = f && f->type == PictTypeDirect && f->direct.alphaMask;
	w->pict = XRenderCreatePicture(dpy, w->pixmap, f ? f : format, CPSubwindowMode, &pa);
}

static void shadow_geometry(struct win *w, int *dx, int *dy, int *radius, double *alpha)
{
	int popup = w->kind == KIND_POPUP;
	if (!strcmp(opt_shadow, "horizon")) {
		*radius = popup ? 2 : 3; *dx = popup ? 3 : 4; *dy = popup ? 3 : 4; *alpha = popup ? 0.36 : 0.30;
	} else if (!strcmp(opt_shadow, "glass")) {
		*radius = popup ? 5 : 15; *dx = 0; *dy = popup ? 2 : 3; *alpha = popup ? 0.30 : 0.48;
	} else {
		*radius = popup ? 6 : 14; *dx = 0; *dy = popup ? 2 : 5; *alpha = popup ? 0.24 : 0.34;
	}
}

static int has_shadow(struct win *w)
{
#ifdef SG_MUTANT_NOSHADOWS
	return 0;
#endif
	return opt_shadows && w->kind != KIND_PLAIN && w->w > 4 && w->h > 4 && w->x > OFFSCREEN &&
	       !(w->x <= 0 && w->y <= 0 && w->x + w->w >= dw && w->y + w->h >= dh);   /* full screen: no room for one */
}

/* what a window may paint over, desktop coordinates */
static void extents(struct win *w, XRectangle *r)
{
	int left = w->x, top = w->y, right = w->x + w->w + 2 * w->bw, bottom = w->y + w->h + 2 * w->bw;
	if (has_shadow(w)) {
		int dx, dy, radius;
		double alpha;
		shadow_geometry(w, &dx, &dy, &radius, &alpha);
		if (left > w->x + dx - 2 * radius) left = w->x + dx - 2 * radius;
		if (top > w->y + dy - 2 * radius) top = w->y + dy - 2 * radius;
		if (right < w->x + w->w + 2 * w->bw + dx + 2 * radius) right = w->x + w->w + 2 * w->bw + dx + 2 * radius;
		if (bottom < w->y + w->h + 2 * w->bw + dy + 2 * radius) bottom = w->y + w->h + 2 * w->bw + dy + 2 * radius;
	}
	if (w->wobbling) {   /* the wobble reaches a little past the window */
		float m = 0;
		for (int i = 0; i < GRID * GRID; i++) m = fmaxf(m, fmaxf(fabsf(w->ox[i]), fabsf(w->oy[i])));
		left -= (int)m + 2; top -= (int)m + 2; right += (int)m + 2; bottom += (int)m + 2;
	}
	if (w->anim == ANIM_MINIMIZE || w->anim == ANIM_RESTORE) bottom = dh;   /* down into the taskbar */
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > dw) right = dw;
	if (bottom > dh) bottom = dh;
	r->x = left; r->y = top;
	r->width = right > left ? right - left : 0;
	r->height = bottom > top ? bottom - top : 0;
}

static void damage_rect(int x, int y, int w, int h)
{
	XRectangle r = { x, y, w, h };
	XserverRegion reg = XFixesCreateRegion(dpy, &r, 1);
	XFixesUnionRegion(dpy, damage_all, damage_all, reg);
	XFixesDestroyRegion(dpy, reg);
}

static void damage_win(struct win *w)
{
	XRectangle r;
	extents(w, &r);
	if (r.width && r.height) damage_rect(r.x, r.y, r.width, r.height);
}

/* a blurred rectangle's alpha: the product of two blurred steps */
static Picture make_shadow(int w, int h, int radius, double alpha)
{
	int sw = w + 4 * radius, sh = h + 4 * radius, stride = (sw + 3) & ~3, x, y;
	double sigma = radius > 0 ? radius / 1.6 : 0.5;
	float *fx = malloc(sw * sizeof(float)), *fy = malloc(sh * sizeof(float));
	char *data = calloc(1, (size_t)stride * sh);
	XImage *img;
	Pixmap pm;
	Picture p;
	GC gc;

	if (!fx || !fy || !data) { free(fx); free(fy); free(data); return 0; }
	for (x = 0; x < sw; x++) {
		double c = x + 0.5 - 2 * radius;
		fx[x] = 0.5 * (erf(c / (sqrt(2) * sigma)) - erf((c - w) / (sqrt(2) * sigma)));
	}
	for (y = 0; y < sh; y++) {
		double c = y + 0.5 - 2 * radius;
		fy[y] = 0.5 * (erf(c / (sqrt(2) * sigma)) - erf((c - h) / (sqrt(2) * sigma)));
	}
	for (y = 0; y < sh; y++)
		for (x = 0; x < sw; x++)
			data[y * stride + x] = (char)(int)(255.0 * alpha * fx[x] * fy[y] + 0.5);
	pm = XCreatePixmap(dpy, desktop, sw, sh, 8);
	img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8, ZPixmap, 0, data, sw, sh, 32, stride);
	gc = XCreateGC(dpy, pm, 0, NULL);
	XPutImage(dpy, pm, gc, img, 0, 0, 0, 0, sw, sh);
	XFreeGC(dpy, gc);
	XDestroyImage(img);   /* frees data */
	p = XRenderCreatePicture(dpy, pm, a8_format, 0, NULL);
	XFreePixmap(dpy, pm);
	free(fx);
	free(fy);
	return p;
}

static void add_window(Window id)
{
	XWindowAttributes a;
	struct win *w;

	if (id == canvas || find(id)) return;
	trapped = 0;
	if (!XGetWindowAttributes(dpy, id, &a) || trapped) return;
	if (a.class == InputOnly) return;
	w = new_entry();
	w->id = id;
	w->x = a.x; w->y = a.y; w->w = a.width; w->h = a.height; w->bw = a.border_width;
	w->mapped = a.map_state == IsViewable;
	w->opacity = 0xffffffff;
	XSelectInput(dpy, id, PropertyChangeMask);
	trapped = 0;
	XCompositeRedirectWindow(dpy, id, CompositeRedirectManual);
	XSync(dpy, False);
	w->managed = !trapped;     /* another client redirected it already: it is theirs */
	if (!w->managed) return;
	w->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
	w->kind = window_kind(id);
	w->opacity = prop_card(id, atom_opacity, XA_CARDINAL, 0xffffffff);
	XShapeSelectInput(dpy, id, ShapeNotifyMask);
	get_shape(w);
	if (w->mapped) { get_pictures(w); damage_win(w); }
}

static void forget(struct win *w)
{
	free_pictures(w);
	if (w->shadow) XRenderFreePicture(dpy, w->shadow);
	if (w->shape) XFixesDestroyRegion(dpy, w->shape);
	if (w->damage) { trapped = 0; XDamageDestroy(dpy, w->damage); XSync(dpy, False); }
	*w = wins[--nwins];
}

/* a picture of a window, kept for an animation after the window itself
 * changed (unmapped, minimized); the window's entry gives it up */
static void make_ghost(struct win *w, enum anim anim)
{
	struct win copy = *w, *g;
	w->pict = 0; w->pixmap = 0; w->shadow = 0; w->shape = None;
	copy.damage = 0;
	copy.ghost = 1;
	copy.wobbling = 0;
	copy.anim = anim;
	copy.anim_start = now();
	g = new_entry();     /* may move the array: w is not used after this */
	*g = copy;
}

/* the stacking order, bottom to top: the X server's; a ghost stays right
 * above where its window is */
static void restack(void)
{
	Window root, parent, *children = NULL;
	unsigned int n = 0;
	struct win *sorted;
	char *used;
	int k = 0;

	if (!XQueryTree(dpy, desktop, &root, &parent, &children, &n)) return;
	sorted = malloc((nwins + 1) * sizeof(*sorted));
	used = calloc(nwins + 1, 1);
	for (unsigned int i = 0; i < n; i++) {
		for (int j = 0; j < nwins; j++)
			if (!used[j] && !wins[j].ghost && wins[j].id == children[i]) { sorted[k++] = wins[j]; used[j] = 1; }
		for (int j = 0; j < nwins; j++)
			if (!used[j] && wins[j].ghost && wins[j].id == children[i]) { sorted[k++] = wins[j]; used[j] = 1; }
	}
	for (int j = 0; j < nwins; j++) if (!used[j]) sorted[k++] = wins[j];   /* ghosts of windows gone: on top */
	memcpy(wins, sorted, k * sizeof(*sorted));
	nwins = k;
	free(sorted);
	free(used);
	if (children) XFree(children);
}

/* ---- the wobble: control points on springs, the grabbed one held ------------- */

static void rest_point(struct win *w, int i, float *x, float *y)
{
	*x = w->x + (float)(w->w + 2 * w->bw) * (i % GRID) / (GRID - 1);
	*y = w->y + (float)(w->h + 2 * w->bw) * (i / GRID) / (GRID - 1);
}

/* the window is moving to (nx, ny): the points stay where they were, all but the held one */
static void wobble_moved(struct win *w, int nx, int ny, double t)
{
	int dx = nx - w->x, dy = ny - w->y;
	if (!w->wobbling || t - w->last_move > 0.3) {
		/* a new drag: hold the point nearest the pointer */
		Window r, c;
		int rx, ry, px, py;
		unsigned int mask;
		float best = 1e9;
		if (!w->wobbling) {
			memset(w->ox, 0, sizeof(w->ox)); memset(w->oy, 0, sizeof(w->oy));
			memset(w->vx, 0, sizeof(w->vx)); memset(w->vy, 0, sizeof(w->vy));
		}
		w->anchor = GRID / 2;
		if (XQueryPointer(dpy, desktop, &r, &c, &rx, &ry, &px, &py, &mask))
			for (int i = 0; i < GRID * GRID; i++) {
				float x, y, d;
				rest_point(w, i, &x, &y);
				x += dx; y += dy;     /* where it is after this move */
				d = (x - px) * (x - px) + (y - py) * (y - py);
				if (d < best) { best = d; w->anchor = i; }
			}
		w->wobbling = 1;
	}
	for (int i = 0; i < GRID * GRID; i++)
		if (i != w->anchor) { w->ox[i] -= dx; w->oy[i] -= dy; }
}

static void wobble_step(struct win *w, double dt)
{
	float ax, ay, bx, by;
	int settled = 1;
	if (dt > 0.05) dt = 0.05;
	rest_point(w, w->anchor, &ax, &ay);
	for (int i = 0; i < GRID * GRID; i++) {
		float x, y, dist, k, c = 9.0f;
		if (i == w->anchor) { w->ox[i] = w->oy[i] = w->vx[i] = w->vy[i] = 0; continue; }
		rest_point(w, i, &x, &y);
		dist = sqrtf((x - ax) * (x - ax) + (y - ay) * (y - ay));
		k = 420.0f / (1.0f + dist / 260.0f);      /* the farther from the grip, the softer */
		bx = -k * w->ox[i] - c * w->vx[i];
		by = -k * w->oy[i] - c * w->vy[i];
		w->vx[i] += bx * dt; w->vy[i] += by * dt;
		w->ox[i] += w->vx[i] * dt; w->oy[i] += w->vy[i] * dt;
		if (fabsf(w->ox[i]) > 0.4f || fabsf(w->oy[i]) > 0.4f || fabsf(w->vx[i]) > 4 || fabsf(w->vy[i]) > 4) settled = 0;
	}
	if (settled) {
		w->wobbling = 0;
		memset(w->ox, 0, sizeof(w->ox)); memset(w->oy, 0, sizeof(w->oy));
	}
}

/* a point of the window (u, v in 0..1) where the wobble puts it */
static void wobble_point(struct win *w, float u, float v, double *x, double *y)
{
	float gx = u * (GRID - 1), gy = v * (GRID - 1), fx, fy, ox, oy;
	int i = (int)gx, j = (int)gy;
	if (i >= GRID - 1) i = GRID - 2;
	if (j >= GRID - 1) j = GRID - 2;
	fx = gx - i; fy = gy - j;
	ox = (1 - fx) * (1 - fy) * w->ox[j * GRID + i] + fx * (1 - fy) * w->ox[j * GRID + i + 1] +
	     (1 - fx) * fy * w->ox[(j + 1) * GRID + i] + fx * fy * w->ox[(j + 1) * GRID + i + 1];
	oy = (1 - fx) * (1 - fy) * w->oy[j * GRID + i] + fx * (1 - fy) * w->oy[j * GRID + i + 1] +
	     (1 - fx) * fy * w->oy[(j + 1) * GRID + i] + fx * fy * w->oy[(j + 1) * GRID + i + 1];
	*x = w->x + u * (w->w + 2 * w->bw) + ox;
	*y = w->y + v * (w->h + 2 * w->bw) + oy;
}

/* ---- painting -------------------------------------------------------------- */

static Picture solid(double a)
{
	XRenderColor c = { 0, 0, 0, (unsigned short)(a * 0xffff) };
	return XRenderCreateSolidFill(dpy, &c);
}

static void set_transform(Picture p, double a, double b, double c, double d, double e, double f)
{
	/* source = [a b c; d e f; 0 0 1] * destination */
	XTransform tr = { { { XDoubleToFixed(a), XDoubleToFixed(b), XDoubleToFixed(c) },
	                    { XDoubleToFixed(d), XDoubleToFixed(e), XDoubleToFixed(f) },
	                    { 0, 0, XDoubleToFixed(1) } } };
	XRenderSetPictureTransform(dpy, p, &tr);
}

static void reset_transform(Picture p)
{
	set_transform(p, 1, 0, 0, 0, 1, 0);
	XRenderSetPictureFilter(dpy, p, "nearest", NULL, 0);
}

static double ease(double p) { return p < 0 ? 0 : p > 1 ? 1 : p * p * (3 - 2 * p); }

static double anim_length(struct win *w)
{
	return w->anim == ANIM_MINIMIZE || w->anim == ANIM_RESTORE ? 0.32 : 0.20;
}

/* where a minimizing window goes: the taskbar, under the window */
static void taskbar_target(struct win *w, double *cx, double *top, double *width)
{
	*cx = w->x + (w->w + 2 * w->bw) / 2.0;
	if (*cx < 60) *cx = 60;
	if (*cx > dw - 60) *cx = dw - 60;
	*top = dh - 24;
	*width = 48;
}

/* the window drawn in horizontal strips, each narrowed and lowered on its
 * own way to the taskbar: the lamp (lamp) or a plain shrink (scale);
 * e = 0 at the window, 1 in the taskbar */
static void draw_minimizing(struct win *w, Picture mask, double e)
{
	int ww = w->w + 2 * w->bw, wh = w->h + 2 * w->bw, strips = wh / 5 < 4 ? 4 : wh / 5 > 160 ? 160 : wh / 5;
	double cx, ty, tw, wcx = w->x + ww / 2.0;
	int lamp = !strcmp(opt_minimize, "lamp");

	taskbar_target(w, &cx, &ty, &tw);
	XRenderSetPictureFilter(dpy, w->pict, "bilinear", NULL, 0);
	for (int i = 0; i < strips; i++) {
		double s0 = (double)wh * i / strips, s1 = (double)wh * (i + 1) / strips;
		double t = (s0 + s1) / 2 / wh, k, width, centre, top, bottom;
		if (lamp) {
			/* the bottom goes first, the top follows */
			k = ease(e * 1.7 - (1 - t) * 0.7);
			width = ww + (tw - ww) * k;
			centre = wcx + (cx - wcx) * k;
			top = (w->y + s0) + (ty - (w->y + s0)) * ease(e * 1.35 - (1 - (double)i / strips) * 0.35);
			bottom = (w->y + s1) + (ty + 4 - (w->y + s1)) * ease(e * 1.35 - (1 - (double)(i + 1) / strips) * 0.35);
		} else {
			k = ease(e);
			width = ww + (tw - ww) * k;
			centre = wcx + (cx - wcx) * k;
			top = (w->y + s0) + (ty + (s0 / wh) * 8 - (w->y + s0)) * k;
			bottom = (w->y + s1) + (ty + (s1 / wh) * 8 - (w->y + s1)) * k;
		}
		if (bottom - top < 0.5 || width < 1) continue;
		{
			double left = centre - width / 2, sx = ww / width, sy = (s1 - s0) / (bottom - top);
			int dx0 = (int)floor(left), dy0 = (int)floor(top);
			int dw0 = (int)ceil(left + width) - dx0, dh0 = (int)ceil(bottom) - dy0;
			/* destination (dx0 + u, dy0 + v) shows source ((dx0 + u - left) * sx, s0 + (dy0 + v - top) * sy) */
			set_transform(w->pict, sx, 0, (dx0 - left) * sx, 0, sy, s0 + (dy0 - top) * sy);
			XRenderComposite(dpy, PictOpOver, w->pict, mask, buffer_pict, 0, 0, 0, 0, dx0, dy0, dw0, dh0);
		}
	}
	reset_transform(w->pict);
}

/* the window as a mesh of tiles, each mapped by the wobble's points: two
 * triangles each, added up into a picture of their own (where two triangles
 * share an edge their coverage adds up to whole: no seams), then laid over
 * what is below */
static void draw_wobbling(struct win *w, Picture mask)
{
	int ww = w->w + 2 * w->bw, wh = w->h + 2 * w->bw;
	int tx = ww > 480 ? 8 : 5, ty = wh > 360 ? 8 : 5;
	double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
	int bx, by, bw, bh;
	Pixmap pm;
	Picture tmp;

	for (int j = 0; j <= ty; j++)
		for (int i = 0; i <= tx; i++) {
			double x, y;
			wobble_point(w, (float)i / tx, (float)j / ty, &x, &y);
			minx = fmin(minx, x); miny = fmin(miny, y); maxx = fmax(maxx, x); maxy = fmax(maxy, y);
		}
	bx = (int)floor(minx) - 1; by = (int)floor(miny) - 1;
	bw = (int)ceil(maxx) + 1 - bx; bh = (int)ceil(maxy) + 1 - by;
	if (bw <= 0 || bh <= 0 || bw > 3 * dw || bh > 3 * dh) return;
	pm = XCreatePixmap(dpy, desktop, bw, bh, 32);
	tmp = XRenderCreatePicture(dpy, pm, argb_format, 0, NULL);
	XRenderFillRectangle(dpy, PictOpClear, tmp, &(XRenderColor){ 0, 0, 0, 0 }, 0, 0, bw, bh);
	XRenderSetPictureFilter(dpy, w->pict, "bilinear", NULL, 0);
	for (int j = 0; j < ty; j++)
		for (int i = 0; i < tx; i++) {
			float u0 = (float)i / tx, u1 = (float)(i + 1) / tx, v0 = (float)j / ty, v1 = (float)(j + 1) / ty;
			double x00, y00, x10, y10, x01, y01, x11, y11, s0 = u0 * ww, t0 = v0 * wh, sw = (u1 - u0) * ww, sh = (v1 - v0) * wh;
			double a, b, c, d, det, ia, ib, ic, id;
			XTriangle tri[2];
			wobble_point(w, u0, v0, &x00, &y00);
			wobble_point(w, u1, v0, &x10, &y10);
			wobble_point(w, u0, v1, &x01, &y01);
			wobble_point(w, u1, v1, &x11, &y11);
			/* in the picture's own coordinates */
			x00 -= bx; x10 -= bx; x01 -= bx; x11 -= bx;
			y00 -= by; y10 -= by; y01 -= by; y11 -= by;
			/* destination = (x00, y00) + [a b; c d] * (source - (s0, t0)) */
			a = (x10 - x00) / sw; b = (x01 - x00) / sh; c = (y10 - y00) / sw; d = (y01 - y00) / sh;
			det = a * d - b * c;
			if (fabs(det) < 1e-6) continue;
			ia = d / det; ib = -b / det; ic = -c / det; id = a / det;
			/* source = (s0, t0) + inverse * (destination - (x00, y00)); the
			 * triangles' source origin is put at their first point, so the
			 * transform sees destination coordinates */
			set_transform(w->pict, ia, ib, s0 - ia * x00 - ib * y00, ic, id, t0 - ic * x00 - id * y00);
			tri[0].p1 = (XPointFixed){ XDoubleToFixed(x00), XDoubleToFixed(y00) };
			tri[0].p2 = (XPointFixed){ XDoubleToFixed(x10), XDoubleToFixed(y10) };
			tri[0].p3 = (XPointFixed){ XDoubleToFixed(x11), XDoubleToFixed(y11) };
			tri[1].p1 = (XPointFixed){ XDoubleToFixed(x00), XDoubleToFixed(y00) };
			tri[1].p2 = (XPointFixed){ XDoubleToFixed(x11), XDoubleToFixed(y11) };
			tri[1].p3 = (XPointFixed){ XDoubleToFixed(x01), XDoubleToFixed(y01) };
			XRenderCompositeTriangles(dpy, PictOpAdd, w->pict, tmp, a8_format,
			                          (int)floor(x00), (int)floor(y00), tri, 2);
		}
	reset_transform(w->pict);
	XRenderComposite(dpy, PictOpOver, tmp, mask, buffer_pict, 0, 0, 0, 0, bx, by, bw, bh);
	XRenderFreePicture(dpy, tmp);
	XFreePixmap(dpy, pm);
}

static void draw_shadow(struct win *w, double fade)
{
	int dx, dy, radius, ww = w->w + 2 * w->bw, wh = w->h + 2 * w->bw;
	double alpha;

	shadow_geometry(w, &dx, &dy, &radius, &alpha);
	if (!w->shadow || w->shadow_w != ww || w->shadow_h != wh || w->shadow_kind != (int)w->kind ||
	    w->shadow_style != opt_shadow[0]) {
		if (w->shadow) XRenderFreePicture(dpy, w->shadow);
		w->shadow = make_shadow(ww, wh, radius, alpha);
		w->shadow_w = ww; w->shadow_h = wh; w->shadow_kind = w->kind; w->shadow_style = opt_shadow[0];
	}
	if (!w->shadow) return;
	if (fade < 1) {
		/* the shadow's alpha times the fade, in black */
		Pixmap pm = XCreatePixmap(dpy, desktop, ww + 4 * radius, wh + 4 * radius, 8);
		Picture tmp = XRenderCreatePicture(dpy, pm, a8_format, 0, NULL), f = solid(fade);
		XRenderComposite(dpy, PictOpSrc, w->shadow, f, tmp, 0, 0, 0, 0, 0, 0, ww + 4 * radius, wh + 4 * radius);
		XRenderComposite(dpy, PictOpOver, black_pict, tmp, buffer_pict, 0, 0, 0, 0,
		                 w->x + dx - 2 * radius, w->y + dy - 2 * radius, ww + 4 * radius, wh + 4 * radius);
		XRenderFreePicture(dpy, f);
		XRenderFreePicture(dpy, tmp);
		XFreePixmap(dpy, pm);
	} else
		XRenderComposite(dpy, PictOpOver, black_pict, w->shadow, buffer_pict, 0, 0, 0, 0,
		                 w->x + dx - 2 * radius, w->y + dy - 2 * radius, ww + 4 * radius, wh + 4 * radius);
}

/* one window, over what is below it; 1 while it is still moving, -1 when a
 * ghost's animation is over */
static int draw_window(struct win *w, double t)
{
	int ww = w->w + 2 * w->bw, wh = w->h + 2 * w->bw, busy = 0;
	double fade = 1.0, zoom = 1.0, minimize = -1, opacity = w->opacity / 4294967295.0;
	Picture mask = None;

	if (w->anim != ANIM_NONE) {
		double p = (t - w->anim_start) / anim_length(w);
		if (p >= 1) {
			if (w->ghost) return -1;
			w->anim = ANIM_NONE;
		} else {
			busy = 1;
			if (p < 0) p = 0;
			if (w->anim == ANIM_OPEN || w->anim == ANIM_CLOSE) {
				double e = ease(w->anim == ANIM_OPEN ? p : 1 - p);
				fade = e;
				if (!strcmp(opt_open, "zoom")) zoom = 0.86 + 0.14 * e;
			} else
				minimize = w->anim == ANIM_MINIMIZE ? p : 1 - p;
		}
	}
	if (w->x <= OFFSCREEN && minimize < 0) return busy;   /* minimized */
	if (opt_moving && opt_anim && w->kind == KIND_FRAMED && t - w->last_move < 0.25) {
		fade *= 0.80;   /* translucent while moved */
		busy = 1;
	}
	if (w->wobbling) busy = 1;
	if (has_shadow(w) && zoom == 1.0 && minimize < 0 && !w->wobbling) draw_shadow(w, fade * opacity);
	if (opacity < 1.0 || fade < 1.0) mask = solid(fade * opacity);

	if (minimize >= 0)
		draw_minimizing(w, mask, minimize);
	else if (w->wobbling)
		draw_wobbling(w, mask);
	else if (zoom != 1.0) {
		/* about the middle */
		XRenderSetPictureFilter(dpy, w->pict, "bilinear", NULL, 0);
		set_transform(w->pict, 1 / zoom, 0, -(ww - ww * zoom) / 2 / zoom, 0, 1 / zoom, -(wh - wh * zoom) / 2 / zoom);
		XRenderComposite(dpy, PictOpOver, w->pict, mask, buffer_pict, 0, 0, 0, 0, w->x, w->y, ww, wh);
		reset_transform(w->pict);
	} else {
		/* clipped to its shape */
		XserverRegion clip = XFixesCreateRegion(dpy, NULL, 0);
		if (w->shape) {
			XFixesCopyRegion(dpy, clip, w->shape);
			XFixesTranslateRegion(dpy, clip, w->x + w->bw, w->y + w->bw);
		} else
			XFixesSetRegion(dpy, clip, &(XRectangle){ w->x, w->y, ww, wh }, 1);
		XFixesIntersectRegion(dpy, clip, clip, damage_all);
		XFixesSetPictureClipRegion(dpy, buffer_pict, 0, 0, clip);
#ifdef SG_MUTANT_OPAQUE
		XRenderComposite(dpy, PictOpSrc, w->pict, None, buffer_pict, 0, 0, 0, 0, w->x, w->y, ww, wh);
#else
		XRenderComposite(dpy, w->argb || mask ? PictOpOver : PictOpSrc, w->pict, mask, buffer_pict,
		                 0, 0, 0, 0, w->x, w->y, ww, wh);
#endif
		XFixesSetPictureClipRegion(dpy, buffer_pict, 0, 0, damage_all);
		XFixesDestroyRegion(dpy, clip);
	}
	if (mask) XRenderFreePicture(dpy, mask);
	return busy;
}

static int paint(void)
{
	double t = now(), dt = last_frame ? t - last_frame : 0.016;
	int busy = 0;

	last_frame = t;
	for (int i = 0; i < nwins; i++)
		if (wins[i].wobbling) { damage_win(&wins[i]); wobble_step(&wins[i], dt); damage_win(&wins[i]); }
	XFixesSetPictureClipRegion(dpy, buffer_pict, 0, 0, damage_all);
	if (bg_pict)
		XRenderComposite(dpy, PictOpSrc, bg_pict, None, buffer_pict, 0, 0, 0, 0, 0, 0, dw, dh);
	else {
		XRenderColor grey = { 0x2400, 0x6e00, 0x9400, 0xffff };
		XRenderFillRectangle(dpy, PictOpSrc, buffer_pict, &grey, 0, 0, dw, dh);
	}
	for (int i = 0; i < nwins; i++) {
		struct win *w = &wins[i];
		int r;
		if ((!w->mapped && !w->ghost) || !w->pict) continue;
		if ((r = draw_window(w, t)) < 0) { damage_win(w); forget(w); i--; continue; }
		busy |= r;
	}
	XFixesSetPictureClipRegion(dpy, canvas_pict, 0, 0, damage_all);
	XRenderComposite(dpy, PictOpSrc, buffer_pict, None, canvas_pict, 0, 0, 0, 0, 0, 0, dw, dh);
	XFixesSetRegion(dpy, damage_all, NULL, 0);
	frames++;
	if (busy)   /* the moving windows' area again next frame */
		for (int i = 0; i < nwins; i++)
			if (wins[i].anim != ANIM_NONE || wins[i].wobbling || (opt_moving && t - wins[i].last_move < 0.3)) damage_win(&wins[i]);
	return busy;
}

static void dump(void)
{
	FILE *f;
	char tmp[600];
	if (!dump_path) return;
	snprintf(tmp, sizeof(tmp), "%s.part", dump_path);
	if (!(f = fopen(tmp, "w"))) return;
	fprintf(f, "desktop=0x%lx canvas=0x%lx size=%dx%d background=%d frames=%lu\n", desktop, canvas, dw, dh, bg_pict != 0, frames);
	fprintf(f, "settings shadows=%d shadow=%s animations=%d open=%s minimize=%s wobbly=%d moving=%d\n",
	        opt_shadows, opt_shadow, opt_anim, opt_open, opt_minimize, opt_wobbly, opt_moving);
	for (int i = 0; i < nwins; i++) {
		float m = 0;
		for (int j = 0; j < GRID * GRID; j++) m = fmaxf(m, fmaxf(fabsf(wins[i].ox[j]), fabsf(wins[i].oy[j])));
		fprintf(f, "win 0x%lx %d,%d %dx%d mapped=%d managed=%d argb=%d kind=%d opacity=%lu anim=%d ghost=%d wobble=%.1f shadow=%d\n",
		        wins[i].id, wins[i].x, wins[i].y, wins[i].w, wins[i].h, wins[i].mapped, wins[i].managed, wins[i].argb,
		        wins[i].kind, wins[i].opacity, wins[i].anim, wins[i].ghost, m, has_shadow(&wins[i]) && wins[i].mapped);
	}
	fclose(f);
	rename(tmp, dump_path);
}

/* ---- the desktop ---------------------------------------------------------- */

static Window find_desktop(Window w, int level)
{
	Window root, parent, *children = NULL, found = 0;
	unsigned int n = 0;
	char *name = NULL;

	if (XFetchName(dpy, w, &name) && name) {
		int is = !strcmp(name, "shell - Wine Desktop");
		XFree(name);
		if (is) return w;
	}
	if (level > 1 || !XQueryTree(dpy, w, &root, &parent, &children, &n)) return 0;
	for (unsigned int i = 0; i < n && !found; i++) found = find_desktop(children[i], level + 1);
	if (children) XFree(children);
	return found;
}

static void load_background(void)
{
	Pixmap pm = prop_card(desktop, atom_bgpixmap, XA_PIXMAP, 0);
	if (bg_pict) XRenderFreePicture(dpy, bg_pict);
	bg_pict = 0;
	if (!pm) return;
	trapped = 0;
	bg_pict = XRenderCreatePicture(dpy, pm, format, 0, NULL);
	XSync(dpy, False);
	if (trapped) bg_pict = 0;
}

static void setup_canvas(void)
{
	XWindowAttributes a;
	XSetWindowAttributes swa = { .background_pixmap = None };
	XRenderPictureAttributes pa = { .subwindow_mode = IncludeInferiors };

	XGetWindowAttributes(dpy, desktop, &a);
	dw = a.width; dh = a.height;
	visual = a.visual;
	depth = a.depth;
	format = XRenderFindVisualFormat(dpy, visual);
	if (canvas) XDestroyWindow(dpy, canvas);
	canvas = XCreateWindow(dpy, desktop, 0, 0, dw, dh, 0, depth, InputOutput, visual, CWBackPixmap, &swa);
	XStoreName(dpy, canvas, "sg-deskcomp");
	/* takes no input: clicks go to the windows below, where they are */
	XShapeCombineRectangles(dpy, canvas, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
	if (buffer_pict) XRenderFreePicture(dpy, buffer_pict);
	if (buffer_pixmap) XFreePixmap(dpy, buffer_pixmap);
	if (canvas_pict) XRenderFreePicture(dpy, canvas_pict);
	buffer_pixmap = XCreatePixmap(dpy, canvas, dw, dh, depth);
	buffer_pict = XRenderCreatePicture(dpy, buffer_pixmap, format, 0, NULL);
	canvas_pict = XRenderCreatePicture(dpy, canvas, format, CPSubwindowMode, &pa);
	XMapRaised(dpy, canvas);
	/* while this canvas lives, winex11 knows windows with alpha are blended
	 * here and takes no backdrop for them (wine-sg 0744) */
	XSetSelectionOwner(dpy, XInternAtom(dpy, "_SG_DESKCOMP_S0", False), canvas, CurrentTime);
	damage_rect(0, 0, dw, dh);
}

static int attach(Window d)
{
	Window root, parent, *children = NULL;
	unsigned int n = 0;

	desktop = d;
	trapped = 0;
	XSelectInput(dpy, desktop, SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);
	XSync(dpy, False);
	if (trapped) { desktop = 0; return 0; }
	setup_canvas();
	load_background();
	if (XQueryTree(dpy, desktop, &root, &parent, &children, &n)) {
		for (unsigned int i = 0; i < n; i++) add_window(children[i]);
		if (children) XFree(children);
	}
	restack();
	return 1;
}

static void detach(void)
{
	while (nwins) { wins[0].damage = 0; forget(&wins[0]); }
	if (bg_pict) XRenderFreePicture(dpy, bg_pict);
	if (buffer_pict) XRenderFreePicture(dpy, buffer_pict);
	if (canvas_pict) XRenderFreePicture(dpy, canvas_pict);
	if (buffer_pixmap) XFreePixmap(dpy, buffer_pixmap);
	bg_pict = buffer_pict = canvas_pict = 0;
	buffer_pixmap = 0;
	canvas = 0;
	desktop = 0;
}

static void on_configure(XConfigureEvent *ce)
{
	struct win *w = find(ce->window);
	double t = now();
	int moved, resized, minimizing, restoring;

	if (!w) return;
	moved = w->x != ce->x || w->y != ce->y;
	resized = w->w != ce->width || w->h != ce->height;
	minimizing = w->x > OFFSCREEN && ce->x <= OFFSCREEN;
	restoring = w->x <= OFFSCREEN && ce->x > OFFSCREEN;
	damage_win(w);
	if (minimizing && w->mapped && w->pict && want_minimize() && w->kind == KIND_FRAMED) {
		make_ghost(w, ANIM_MINIMIZE);
		w = find(ce->window);
	}
	if (moved && !resized && !minimizing && !restoring && w->mapped && w->kind == KIND_FRAMED) {
		if (want_wobbly()) wobble_moved(w, ce->x, ce->y, t);
		w->last_move = t;
	}
	if (resized && w->wobbling) w->wobbling = 0;
	w->x = ce->x; w->y = ce->y;
	w->w = ce->width; w->h = ce->height; w->bw = ce->border_width;
	if ((resized || !w->pict) && w->mapped) get_pictures(w);
	if (restoring && want_minimize() && w->kind == KIND_FRAMED) { w->anim = ANIM_RESTORE; w->anim_start = t; }
	restack();
	damage_win(w);
}

static void on_quit(int sig)
{
	(void)sig;
	quit = 1;
}

int main(int argc, char **argv)
{
	const char *display = NULL;
	Window want = 0;
	int ev, err, major = 0, minor = 2;
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	XRenderColor black = { 0, 0, 0, 0xffff };

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-display") && i + 1 < argc) display = argv[++i];
		else if (!strcmp(argv[i], "-window") && i + 1 < argc) want = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-dump") && i + 1 < argc) dump_path = argv[++i];
		else { fprintf(stderr, "usage: sg-deskcomp [-display DPY] [-window XID] [-dump FILE]\n"); return 2; }
	}
	if (cfg && *cfg) snprintf(conf_path, sizeof(conf_path), "%s/stained-glass/effects.conf", cfg);
	else snprintf(conf_path, sizeof(conf_path), "%s/.config/stained-glass/effects.conf", home ? home : "");
	if (!(dpy = XOpenDisplay(display))) { fprintf(stderr, "sg-deskcomp: no X display\n"); return 1; }
	if (!XCompositeQueryExtension(dpy, &ev, &err) || (XCompositeQueryVersion(dpy, &major, &minor), major == 0 && minor < 2) ||
	    !XDamageQueryExtension(dpy, &damage_event, &damage_error) || !XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error) ||
	    !XRenderQueryExtension(dpy, &ev, &err) || !XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
		fprintf(stderr, "sg-deskcomp: the X server lacks Composite, Damage, XFixes, Render or Shape\n");
		return 1;
	}
	XSetErrorHandler(error_handler);
	signal(SIGTERM, on_quit);
	signal(SIGINT, on_quit);
	signal(SIGHUP, SIG_IGN);
	atom_opacity = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
	atom_shadow = XInternAtom(dpy, "_SG_SHADOW", False);
	atom_bgpixmap = XInternAtom(dpy, "_SG_DESKTOP_PIXMAP", False);
	argb_format = XRenderFindStandardFormat(dpy, PictStandardARGB32);
	a8_format = XRenderFindStandardFormat(dpy, PictStandardA8);
	damage_all = XFixesCreateRegion(dpy, NULL, 0);
	read_settings();
	{
		Pixmap pm = XCreatePixmap(dpy, DefaultRootWindow(dpy), 1, 1, 32);
		XRenderPictureAttributes pa = { .repeat = RepeatNormal };
		black_pict = XRenderCreatePicture(dpy, pm, argb_format, CPRepeat, &pa);
		XRenderFillRectangle(dpy, PictOpSrc, black_pict, &black, 0, 0, 1, 1);
		XFreePixmap(dpy, pm);
	}

	while (!quit) {
		struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
		int busy = 0;

		if (!desktop) {
			Window d = want ? want : find_desktop(DefaultRootWindow(dpy), 0);
			/* without the desktop's picture (an older wine-sg) there is nothing to paint under the windows */
			if (d && prop_card(d, atom_bgpixmap, XA_PIXMAP, 0) && attach(d)) dump();
			else { poll(NULL, 0, 500); XSync(dpy, False); continue; }
		}
		while (XPending(dpy)) {
			XEvent e;
			struct win *w;
			XNextEvent(dpy, &e);
			if (e.type == damage_event + XDamageNotify) {
				XDamageNotifyEvent *de = (XDamageNotifyEvent *)&e;
				XserverRegion parts = XFixesCreateRegion(dpy, NULL, 0);
				XDamageSubtract(dpy, de->damage, None, parts);
				if ((w = find(de->drawable)) && !w->wobbling) {
					XFixesTranslateRegion(dpy, parts, w->x + w->bw, w->y + w->bw);
					XFixesUnionRegion(dpy, damage_all, damage_all, parts);
				} else if (w)
					damage_win(w);
				XFixesDestroyRegion(dpy, parts);
				continue;
			}
			if (e.type == shape_event + ShapeNotify) {
				if ((w = find(((XShapeEvent *)&e)->window))) { damage_win(w); get_shape(w); damage_win(w); }
				continue;
			}
			switch (e.type) {
			case CreateNotify:
				if (e.xcreatewindow.parent == desktop) { add_window(e.xcreatewindow.window); restack(); }
				break;
			case DestroyNotify:
				if (e.xdestroywindow.window == desktop) { detach(); break; }
				if ((w = find(e.xdestroywindow.window))) {
					damage_win(w);
					w->damage = 0;     /* gone with the window */
					forget(w);
				}
				break;
			case MapNotify:
				if ((w = find(e.xmap.window))) {
					w->mapped = 1;
					w->kind = window_kind(w->id);
					get_shape(w);
					get_pictures(w);
					if (want_open() && w->kind != KIND_PLAIN && w->x > OFFSCREEN) { w->anim = ANIM_OPEN; w->anim_start = now(); }
					restack();
					damage_win(w);
				}
				break;
			case UnmapNotify:
				if ((w = find(e.xunmap.window))) {
					damage_win(w);
					w->mapped = 0;
					w->wobbling = 0;
					if (want_open() && w->kind != KIND_PLAIN && w->pict && w->x > OFFSCREEN) {
						make_ghost(w, ANIM_CLOSE);   /* the last picture, fading out */
						restack();
					} else
						free_pictures(w);
				}
				break;
			case ConfigureNotify:
				if (e.xconfigure.window == desktop) {
					if (e.xconfigure.width != dw || e.xconfigure.height != dh) setup_canvas();
					break;
				}
				on_configure(&e.xconfigure);
				break;
			case ReparentNotify:
				if (e.xreparent.parent == desktop) { add_window(e.xreparent.window); restack(); }
				else if ((w = find(e.xreparent.window))) { damage_win(w); forget(w); }
				break;
			case PropertyNotify:
				if (e.xproperty.window == desktop && e.xproperty.atom == atom_bgpixmap) {
					load_background();
					damage_rect(0, 0, dw, dh);
				} else if ((w = find(e.xproperty.window))) {
					if (e.xproperty.atom == atom_opacity) w->opacity = prop_card(w->id, atom_opacity, XA_CARDINAL, 0xffffffff);
					else if (e.xproperty.atom == atom_shadow) w->kind = window_kind(w->id);
					damage_win(w);
				}
				break;
			}
		}
		if (!desktop) continue;
		read_settings();
		{
			XRectangle ext;
			int count = 0;
			XRectangle *rects = XFixesFetchRegionAndBounds(dpy, damage_all, &count, &ext);
			if (rects) XFree(rects);
			if (count) { busy = paint(); XFlush(dpy); dump(); }
			else last_frame = 0;
		}
		if (!XPending(dpy)) poll(&pfd, 1, busy ? 16 : 1000);
	}
	/* hand the windows back to X */
	if (desktop) {
		for (int i = 0; i < nwins; i++)
			if (wins[i].managed && !wins[i].ghost) XCompositeUnredirectWindow(dpy, wins[i].id, CompositeRedirectManual);
		if (canvas) XDestroyWindow(dpy, canvas);
	}
	XCloseDisplay(dpy);
	return 0;
}
