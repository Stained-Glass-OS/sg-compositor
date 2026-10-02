/*
 * sg-compositor: title bars for Linux programs' windows.
 *
 * Wine draws its own windows' frames; a Linux program's X11 window -- the
 * Linux Terminal (Administrator)'s xterm, the RDP client's dialogs -- had
 * none: no title, no way to move it, close it or make it larger. Here such a
 * window gets Windows 10's title bar, drawn by the compositor above it: the
 * title, Minimize (its taskbar button -- the shell's XWINDOWS list -- brings
 * it back), Maximize and Close, a one-pixel border; drag the bar to move the
 * window, double-click it to maximize. Close asks the program to close
 * (WM_DELETE_WINDOW), as a title bar's Close does.
 *
 * Not for Wine's windows (override-redirect), an elevated program's (Wine
 * draws those), a full-screen window, or one that asks for no decorations
 * (_MOTIF_WM_HINTS -- an RDP session's own window).
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include "decor.h"

#include <drm_fourcc.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xwayland.h>
#include <xcb/xcb.h>

#include "seat.h"
#include "server.h"
#include "view.h"
#include "xwayland.h"
#include "session_x11.h"

/* as every other window's caption buttons (Wine's, the theme's WindowMetrics:
 * square, 30 px), and its #e3e3e3 edge (David 2026-10-01: SG Office's and the
 * Linux programs' title bars looked different) */
#ifdef SG_MUTANT_WIDE_BUTTONS
#define BUTTON_W 46
#else
#define BUTTON_W 30
#endif
#define TASKBAR_H 40 /* the shell's taskbar, at the bottom: a maximized window stops above it */
#define ICON_PX 16   /* the program's icon at the bar's left, as Wine's windows have theirs */
#define ICON_X 7
#define TITLE_X 26   /* where Wine's windows' titles start */
#define DOUBLE_CLICK_MS 400

enum part { PART_NONE, PART_BAR, PART_MIN, PART_MAX, PART_CLOSE };

struct cg_decor {
	struct wl_list link; /* decors */
	struct cg_view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *bar;
	struct wlr_scene_rect *border[4];
	int width, height;
	bool active;
	enum part hover;
	struct wl_listener commit;
	struct wl_listener set_title;
	uint32_t icon[ICON_PX * ICON_PX]; /* the program's icon, ARGB, when has_icon */
	bool has_icon;
	int icon_tries;                   /* programs often set it after mapping */
};

static struct wl_list decors = {&decors, &decors};

/* the pressed part, and a move in progress */
static struct {
	struct cg_decor *decor;
	enum part part;
	bool moving;
	double cx, cy;
	int x, y;
	uint32_t last_bar_time;
	struct cg_view *last_bar_view;
} press;

/* ---- pixels ------------------------------------------------------------- */

struct pixels {
	struct wlr_buffer base;
	uint32_t *data;
	size_t stride;
};

static void
pixels_destroy(struct wlr_buffer *buffer)
{
	struct pixels *p = wl_container_of(buffer, p, base);
	free(p->data);
	free(p);
}

static bool
pixels_begin(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
	struct pixels *p = wl_container_of(buffer, p, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = p->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = p->stride;
	return true;
}

static void
pixels_end(struct wlr_buffer *buffer)
{
	(void) buffer;
}

static const struct wlr_buffer_impl pixels_impl = {
	.destroy = pixels_destroy,
	.begin_data_ptr_access = pixels_begin,
	.end_data_ptr_access = pixels_end,
};

static void
fill(struct pixels *p, int w, int x0, int y0, int x1, int y1, uint32_t color)
{
	for (int y = y0 < 0 ? 0 : y0; y < y1 && y < DECOR_TITLE_H; y++) {
		for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) {
			p->data[y * w + x] = color;
		}
	}
}

static void
put(struct pixels *p, int w, int x, int y, uint32_t color)
{
	if (x >= 0 && x < w && y >= 0 && y < DECOR_TITLE_H) {
		p->data[y * w + x] = color;
	}
}

/* color over what is there, by coverage 0..255 (all opaque) */
static void
blend(struct pixels *p, int w, int x, int y, uint32_t color, unsigned cover)
{
	if (x < 0 || x >= w || y < 0 || y >= DECOR_TITLE_H || !cover) {
		return;
	}
	uint32_t bg = p->data[y * w + x], out = 0xff000000;
	for (int s = 0; s < 24; s += 8) {
		unsigned a = (color >> s) & 0xff, b = (bg >> s) & 0xff;
		out |= ((a * cover + b * (255 - cover)) / 255) << s;
	}
	p->data[y * w + x] = out;
}

/* ---- text --------------------------------------------------------------- */

static FT_Library ft;
static FT_Face face;
static bool ft_tried;

static bool
font_ready(void)
{
	static const char *fonts[] = {
		"/usr/share/fonts/opentype/inter/Inter-Regular.otf",
		"/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		NULL,
	};
	if (face || ft_tried) {
		return face != NULL;
	}
	ft_tried = true;
	if (FT_Init_FreeType(&ft)) {
		return false;
	}
	for (int i = 0; fonts[i]; i++) {
		if (!FT_New_Face(ft, fonts[i], 0, &face)) {
			FT_Set_Pixel_Sizes(face, 0, 12);
			return true;
		}
	}
	face = NULL;
	return false;
}

static uint32_t
utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *) *s;
	uint32_t c = *p++;
	int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	if (extra) {
		c &= 0x3f >> extra;
	}
	while (extra-- && (*p & 0xc0) == 0x80) {
		c = c << 6 | (*p++ & 0x3f);
	}
	*s = (const char *) p;
	return c;
}

static int
text_width(const char *s, int n)
{
	int w = 0;
	const char *end = s + n;
	while (s < end && *s) {
		if (!FT_Load_Char(face, utf8_next(&s), FT_LOAD_DEFAULT)) {
			w += face->glyph->advance.x >> 6;
		}
	}
	return w;
}

static int
draw_text(struct pixels *p, int w, int x, int baseline, const char *s, int n, uint32_t color)
{
	const char *end = s + n;
	while (s < end && *s) {
		if (FT_Load_Char(face, utf8_next(&s), FT_LOAD_RENDER)) {
			continue;
		}
		FT_GlyphSlot g = face->glyph;
		for (unsigned r = 0; r < g->bitmap.rows; r++) {
			for (unsigned c = 0; c < g->bitmap.width; c++) {
				blend(p, w, x + g->bitmap_left + (int) c, baseline - g->bitmap_top + (int) r, color,
				      g->bitmap.buffer[r * g->bitmap.pitch + c]);
			}
		}
		x += g->advance.x >> 6;
	}
	return x;
}

/* the title in the room there is, with "..." when it does not fit */
static void
draw_title(struct pixels *p, int w, int x, int room, const char *title, uint32_t color)
{
	int n = strlen(title), dots;
	if (!title[0] || room <= 0 || !font_ready()) {
		return;
	}
	int baseline = DECOR_TITLE_H / 2 + 4;
	if (text_width(title, n) <= room) {
		draw_text(p, w, x, baseline, title, n, color);
		return;
	}
	dots = text_width("...", 3);
	while (n > 0 && text_width(title, n) + dots > room) {
		do {
			n--;
		} while (n > 0 && ((unsigned char) title[n] & 0xc0) == 0x80);
	}
	x = draw_text(p, w, x, baseline, title, n, color);
	draw_text(p, w, x, baseline, "...", 3, color);
}

/* ---- the program's icon --------------------------------------------------- */

/* _NET_WM_ICON: the size nearest above 16 px (else the largest), boxed down
 * to 16 x 16; read once it is there (a few renders' worth of tries) */
static void
fetch_icon(struct cg_decor *d, struct wlr_xwayland_surface *xs)
{
	static xcb_atom_t net_wm_icon;
	xcb_connection_t *c;
	xcb_get_property_reply_t *r;
	uint32_t *v, n, best = 0, bw = 0, bh = 0;

#ifdef SG_MUTANT_NO_ICON
	return;
#endif
	if (d->has_icon || d->icon_tries >= 8 || !d->view->server->xwayland) {
		return;
	}
	d->icon_tries++;
	if (!(c = wlr_xwayland_get_xwm_connection(d->view->server->xwayland))) {
		return;
	}
	if (!net_wm_icon) {
		xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 12, "_NET_WM_ICON"), NULL);
		if (!a) {
			return;
		}
		net_wm_icon = a->atom;
		free(a);
	}
	r = xcb_get_property_reply(c, xcb_get_property(c, 0, xs->window_id, net_wm_icon, XCB_ATOM_CARDINAL, 0, 1 << 20), NULL);
	if (!r) {
		return;
	}
	v = xcb_get_property_value(r);
	n = xcb_get_property_value_length(r) / 4;
	/* entries: width, height, then width * height ARGB pixels */
	for (uint32_t i = 0; i + 2 <= n;) {
		uint32_t w = v[i], h = v[i + 1];
		if (!w || !h || w > 1024 || h > 1024 || i + 2 + (uint64_t) w * h > n) {
			break;
		}
		if (!bw || (w >= ICON_PX && (bw < ICON_PX || w < bw)) || (bw < ICON_PX && w > bw)) {
			best = i; bw = w; bh = h;
		}
		i += 2 + w * h;
	}
	if (bw) {
		const uint32_t *px = v + best + 2;
		for (int y = 0; y < ICON_PX; y++) {
			for (int x = 0; x < ICON_PX; x++) {
				/* the source pixels under this one, averaged (premultiplied) */
				uint32_t x0 = x * bw / ICON_PX, x1 = (x + 1) * bw / ICON_PX, y0 = y * bh / ICON_PX,
					 y1 = (y + 1) * bh / ICON_PX, cnt = 0, sa = 0, sr = 0, sg = 0, sb = 0;
				if (x1 <= x0) x1 = x0 + 1;
				if (y1 <= y0) y1 = y0 + 1;
				for (uint32_t yy = y0; yy < y1 && yy < bh; yy++) {
					for (uint32_t xx = x0; xx < x1 && xx < bw; xx++) {
						uint32_t p = px[yy * bw + xx], a = p >> 24;
						sa += a;
						sr += ((p >> 16) & 0xff) * a / 255;
						sg += ((p >> 8) & 0xff) * a / 255;
						sb += (p & 0xff) * a / 255;
						cnt++;
					}
				}
				d->icon[y * ICON_PX + x] = cnt ? (sa / cnt) << 24 | (sr / cnt) << 16 | (sg / cnt) << 8 | (sb / cnt) : 0;
			}
		}
		d->has_icon = true;
	}
	free(r);
}

/* ---- the bar ------------------------------------------------------------ */

static struct wlr_xwayland_surface *
xsurface(struct cg_view *view)
{
	return xwayland_view_from_view(view)->xwayland_surface;
}

static struct cg_decor *
decor_of(struct cg_view *view)
{
	struct cg_decor *d;
	wl_list_for_each (d, &decors, link) {
		if (d->view == view) {
			return d;
		}
	}
	return NULL;
}

/* Dark mode (Settings > Personalization > Colors), as sg-settingsctl writes
 * it for GTK: gtk-application-prefer-dark-theme in the user's gtk-3.0
 * settings.ini. A Linux program's bar was light in dark mode until Wine
 * framed it -- Kate's title bar went light, then dark (David 2026-10-02).
 * Read again when the file changes. */
static bool
dark_mode(void)
{
	static bool dark;
	static struct timespec seen;
	char path[512];
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	struct stat st;

#ifdef SG_MUTANT_DECOR_ALWAYS_LIGHT
	return false;
#endif
	if (cfg && *cfg) {
		snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", cfg);
	} else if (home) {
		snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini", home);
	} else {
		return false;
	}
	if (stat(path, &st) != 0) {
		return dark = false;
	}
	if (st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec) {
		return dark;
	}
	seen = st.st_mtim;
	dark = false;
	FILE *f = fopen(path, "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			char *v = strchr(line, '=');
			if (v && !strncmp(line, "gtk-application-prefer-dark-theme", 33)) {
				v++;
				while (*v == ' ') {
					v++;
				}
				dark = !strncmp(v, "true", 4) || *v == '1';
			}
		}
		fclose(f);
	}
	return dark;
}

static void
render(struct cg_decor *d)
{
	struct wlr_xwayland_surface *xs = xsurface(d->view);
	int w = d->width;
	bool dark = dark_mode();
	uint32_t bg = dark ? 0xff202020 : 0xffffffff;
	uint32_t fg = dark ? (d->active ? 0xffffffff : 0xff8a8a8a) : (d->active ? 0xff000000 : 0xff999999);
	uint32_t hover = dark ? 0xff3a3a3a : 0xffe5e5e5;
#ifdef SG_MUTANT_WIDE_BUTTONS
	uint32_t border = d->active ? 0xff707070 : 0xffaaaaaa;
#else
	uint32_t border = dark ? 0xff404040 : 0xffe3e3e3;
#endif

	if (w < 1) {
		return;
	}
	struct pixels *p = calloc(1, sizeof(*p));
	if (!p || !(p->data = malloc((size_t) w * DECOR_TITLE_H * 4))) {
		free(p);
		return;
	}
	p->stride = (size_t) w * 4;
	fill(p, w, 0, 0, w, DECOR_TITLE_H, bg);

	/* Close: an X, white on red under the pointer */
	int cx = w - BUTTON_W / 2, cy = DECOR_TITLE_H / 2;
	uint32_t close_fg = fg;
	if (d->hover == PART_CLOSE) {
		fill(p, w, w - BUTTON_W, 0, w, DECOR_TITLE_H, 0xffe81123);
		close_fg = 0xffffffff;
	}
	for (int i = -5; i <= 5; i++) {
		put(p, w, cx + i, cy + i, close_fg);
		put(p, w, cx + i, cy - i, close_fg);
	}

	/* Maximize: a square; Restore: two */
	int mx = w - BUTTON_W - BUTTON_W / 2;
	if (d->hover == PART_MAX) {
		fill(p, w, w - 2 * BUTTON_W, 0, w - BUTTON_W, DECOR_TITLE_H, hover);
	}
	if (!d->view->maximized) {
		for (int i = -5; i <= 5; i++) {
			put(p, w, mx + i, cy - 5, fg);
			put(p, w, mx + i, cy + 5, fg);
			put(p, w, mx - 5, cy + i, fg);
			put(p, w, mx + 5, cy + i, fg);
		}
	} else {
		/* the front square, and the back one's top and right edges beside it */
		for (int i = 0; i <= 8; i++) {
			put(p, w, mx - 5 + i, cy - 3, fg);
			put(p, w, mx - 5 + i, cy + 5, fg);
			put(p, w, mx - 5, cy - 3 + i, fg);
			put(p, w, mx + 3, cy - 3 + i, fg);
			put(p, w, mx - 3 + i, cy - 5, fg);
			put(p, w, mx + 5, cy - 5 + i, fg);
		}
		put(p, w, mx - 3, cy - 4, fg);
		put(p, w, mx + 4, cy + 3, fg);
	}

	/* Minimize: a short line */
	int nx = w - 2 * BUTTON_W - BUTTON_W / 2;
	if (d->hover == PART_MIN) {
		fill(p, w, w - 3 * BUTTON_W, 0, w - 2 * BUTTON_W, DECOR_TITLE_H, hover);
	}
	for (int i = -5; i <= 5; i++) {
		put(p, w, nx + i, cy, fg);
	}

	fetch_icon(d, xs);
	if (d->has_icon) {
		/* over the bar: premultiplied over */
		int iy = (DECOR_TITLE_H - ICON_PX) / 2;
		for (int y = 0; y < ICON_PX; y++) {
			for (int x = 0; x < ICON_PX && ICON_X + x < w; x++) {
				uint32_t s = d->icon[y * ICON_PX + x], a = s >> 24, b = p->data[(iy + y) * w + ICON_X + x];
				uint32_t r = ((s >> 16) & 0xff) + ((b >> 16) & 0xff) * (255 - a) / 255;
				uint32_t g = ((s >> 8) & 0xff) + ((b >> 8) & 0xff) * (255 - a) / 255;
				uint32_t bl = (s & 0xff) + (b & 0xff) * (255 - a) / 255;
				p->data[(iy + y) * w + ICON_X + x] = 0xff000000 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (bl > 255 ? 255 : bl);
			}
		}
	}
	draw_title(p, w, d->has_icon ? TITLE_X : 12, w - 3 * BUTTON_W - 20 - (d->has_icon ? TITLE_X - 12 : 0),
		   xs->title ? xs->title : "", fg);

	wlr_buffer_init(&p->base, &pixels_impl, w, DECOR_TITLE_H);
	wlr_scene_buffer_set_buffer(d->bar, &p->base);
	wlr_buffer_drop(&p->base);

	/* the border, around the bar and the window */
	float c[4] = {((border >> 16) & 0xff) / 255.0f, ((border >> 8) & 0xff) / 255.0f, (border & 0xff) / 255.0f, 1};
	int h = d->height;
	wlr_scene_rect_set_size(d->border[0], w + 2, 1);
	wlr_scene_node_set_position(&d->border[0]->node, -1, -DECOR_TITLE_H - 1);
	wlr_scene_rect_set_size(d->border[1], w + 2, 1);
	wlr_scene_node_set_position(&d->border[1]->node, -1, h);
	wlr_scene_rect_set_size(d->border[2], 1, h + DECOR_TITLE_H);
	wlr_scene_node_set_position(&d->border[2]->node, -1, -DECOR_TITLE_H);
	wlr_scene_rect_set_size(d->border[3], 1, h + DECOR_TITLE_H);
	wlr_scene_node_set_position(&d->border[3]->node, w, -DECOR_TITLE_H);
	for (int i = 0; i < 4; i++) {
		wlr_scene_rect_set_color(d->border[i], c);
	}
}

static void
handle_commit(struct wl_listener *listener, void *data)
{
	struct cg_decor *d = wl_container_of(listener, d, commit);
	struct wlr_surface *surface = data;
	(void) surface;
	struct wlr_surface *s = d->view->wlr_surface;
	if (s && (s->current.width != d->width || s->current.height != d->height)) {
		d->width = s->current.width;
		d->height = s->current.height;
		render(d);
	}
}

static void
handle_set_title(struct wl_listener *listener, void *data)
{
	struct cg_decor *d = wl_container_of(listener, d, set_title);
	(void) data;
	render(d);
}

void
decor_create(struct cg_view *view)
{
	struct wlr_xwayland_surface *xs = xsurface(view);
	struct cg_decor *d;

	if (!view->scene_tree || view->elevated || xs->override_redirect || xs->fullscreen ||
	    xwayland_view_is_shell_desktop(view) ||
	    xs->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL || decor_of(view)) {
		return;
	}
	if (!(d = calloc(1, sizeof(*d)))) {
		return;
	}
	d->view = view;
	d->tree = wlr_scene_tree_create(view->scene_tree);
	d->bar = d->tree ? wlr_scene_buffer_create(d->tree, NULL) : NULL;
	if (!d->bar) {
		if (d->tree) {
			wlr_scene_node_destroy(&d->tree->node);
		}
		free(d);
		return;
	}
	wlr_scene_node_set_position(&d->bar->node, 0, -DECOR_TITLE_H);
	for (int i = 0; i < 4; i++) {
		d->border[i] = wlr_scene_rect_create(d->tree, 1, 1, (float[4]) {0.5f, 0.5f, 0.5f, 1});
	}
	d->width = view->wlr_surface->current.width;
	d->height = view->wlr_surface->current.height;
	/* the window may have been given the focus before its bar existed */
	d->active = seat_get_focus(view->server->seat) == view;
	d->commit.notify = handle_commit;
	wl_signal_add(&view->wlr_surface->events.commit, &d->commit);
	d->set_title.notify = handle_set_title;
	wl_signal_add(&xs->events.set_title, &d->set_title);
	wl_list_insert(&decors, &d->link);
	view->decorated = true;
	render(d);
}

bool
decor_has(struct cg_view *view)
{
	struct cg_decor *d;
	wl_list_for_each (d, &decors, link) {
		if (d->view == view) {
			return true;
		}
	}
	return false;
}

void
decor_destroy(struct cg_view *view)
{
	struct cg_decor *d = decor_of(view);
	if (!d) {
		return;
	}
	if (press.decor == d) {
		memset(&press, 0, sizeof(press));
	}
	if (press.last_bar_view == view) {
		press.last_bar_view = NULL;
	}
	wl_list_remove(&d->commit.link);
	wl_list_remove(&d->set_title.link);
	wl_list_remove(&d->link);
	/* its nodes go with the view's scene tree, unless that stays */
	if (view->scene_tree) {
		wlr_scene_node_destroy(&d->tree->node);
	}
	view->decorated = false;
	free(d);
}

void
decor_set_active(struct cg_view *view, bool active)
{
	struct cg_decor *d = decor_of(view);
	if (d && d->active != active) {
		d->active = active;
		render(d);
	}
}

void
decor_set_fullscreen(struct cg_view *view, bool fullscreen)
{
	struct cg_decor *d = decor_of(view);
	if (d) {
		wlr_scene_node_set_enabled(&d->tree->node, !fullscreen);
	}
}

/* ---- input -------------------------------------------------------------- */

static struct cg_decor *
decor_at(struct cg_server *server, double lx, double ly, enum part *part)
{
	double sx, sy;
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, &sx, &sy);
	struct cg_decor *d;

	*part = PART_NONE;
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	wl_list_for_each (d, &decors, link) {
		if (node == &d->bar->node && d->tree->node.enabled) {
			*part = sx >= d->width - BUTTON_W       ? PART_CLOSE
				: sx >= d->width - 2 * BUTTON_W ? PART_MAX
				: sx >= d->width - 3 * BUTTON_W ? PART_MIN
								: PART_BAR;
			return d;
		}
	}
	return NULL;
}

static void
move_view(struct cg_view *view, int x, int y, int w, int h)
{
	struct wlr_box box;
	view_get_layout_box(view, &box);
	view->lx = x;
	view->ly = y;
	view->rx = x - box.x;
	view->ry = y - box.y;
	view->user_placed = true;
	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	}
	wlr_xwayland_surface_configure(xsurface(view), x, y, w, h);
}

static void
toggle_maximize(struct cg_decor *d)
{
	struct cg_view *view = d->view;
	struct wlr_xwayland_surface *xs = xsurface(view);
	struct wlr_box box;

	if (!view->maximized) {
		view->restore = (struct wlr_box) {view->lx, view->ly, xs->width, xs->height};
		view_get_layout_box(view, &box);
		view->maximized = true;
		wlr_xwayland_surface_set_maximized(xs, true);
		move_view(view, box.x, box.y + DECOR_TITLE_H, box.width, box.height - DECOR_TITLE_H - TASKBAR_H);
	} else {
		view->maximized = false;
		wlr_xwayland_surface_set_maximized(xs, false);
		move_view(view, view->restore.x, view->restore.y, view->restore.width, view->restore.height);
	}
	render(d);
}

bool
decor_button(struct cg_seat *seat, bool pressed, uint32_t time_msec)
{
	enum part part;
	struct cg_decor *d;

	if (!pressed) {
		if (!press.decor) {
			return false;
		}
		d = decor_at(seat->server, seat->cursor->x, seat->cursor->y, &part);
		if (d == press.decor && part == press.part) {
			if (part == PART_CLOSE) {
				wlr_xwayland_surface_close(xsurface(d->view));
			} else if (part == PART_MAX) {
				toggle_maximize(d);
			} else if (part == PART_MIN) {
				session_x11_minimize(seat->server, xsurface(d->view)->window_id);
			}
		}
		press.decor = NULL;
		press.part = PART_NONE;
		press.moving = false;
		return true;
	}

	if (!(d = decor_at(seat->server, seat->cursor->x, seat->cursor->y, &part))) {
		return false;
	}
	if (seat_get_focus(seat) != d->view) {
		seat_set_focus(seat, d->view);
	}
	press.decor = d;
	press.part = part;
	if (part == PART_BAR) {
		if (press.last_bar_view == d->view && time_msec - press.last_bar_time < DOUBLE_CLICK_MS) {
			press.last_bar_view = NULL;
			press.decor = NULL;
			toggle_maximize(d);
			return true;
		}
		press.last_bar_view = d->view;
		press.last_bar_time = time_msec;
		press.moving = true;
		press.cx = seat->cursor->x;
		press.cy = seat->cursor->y;
		press.x = d->view->lx;
		press.y = d->view->ly;
	}
	return true;
}

bool
decor_motion(struct cg_seat *seat, double lx, double ly)
{
	enum part part;
	struct cg_decor *d, *over;

	if (press.decor && press.moving) {
		d = press.decor;
		struct wlr_xwayland_surface *xs = xsurface(d->view);
		int x = press.x + (int) (lx - press.cx), y = press.y + (int) (ly - press.cy);
		struct wlr_box box;
		view_get_layout_box(d->view, &box);
		/* the bar stays reachable: never above the top of the screen */
		if (y < box.y + DECOR_TITLE_H) {
			y = box.y + DECOR_TITLE_H;
		}
		if (d->view->maximized) {
			/* dragging a maximized window's bar restores it, under the pointer */
			d->view->maximized = false;
			wlr_xwayland_surface_set_maximized(xs, false);
			press.x = (int) lx - d->view->restore.width / 2;
			press.cx = lx;
			x = press.x;
			move_view(d->view, x, y, d->view->restore.width, d->view->restore.height);
			render(d);
			return true;
		}
		move_view(d->view, x, y, xs->width, xs->height);
		return true;
	}

	over = decor_at(seat->server, lx, ly, &part);
	wl_list_for_each (d, &decors, link) {
		enum part hover = d == over && part != PART_BAR ? part : PART_NONE;
		if (d->hover != hover) {
			d->hover = hover;
			render(d);
		}
	}
	if (over) {
		wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager, "default");
	}
	return false;
}
