/*
 * sg-compositor: title bars for Linux programs' windows.
 *
 * Wine draws its own windows' frames; a Linux program's X11 window -- the
 * Linux Terminal (Administrator)'s xterm, the RDP client's dialogs -- had
 * none: no title, no way to move it, close it or make it larger. Here such a
 * window gets Windows 10's title bar, drawn by the compositor above it: the
 * title, Maximize and Close (the program has no taskbar button, so there is
 * no Minimize to lose it by), a one-pixel border; drag the bar to move the
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
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xwayland.h>

#include "seat.h"
#include "server.h"
#include "view.h"
#include "xwayland.h"

#define BUTTON_W 46
#define TASKBAR_H 40 /* the shell's taskbar, at the bottom: a maximized window stops above it */
#define DOUBLE_CLICK_MS 400

enum part { PART_NONE, PART_BAR, PART_MAX, PART_CLOSE };

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

static void
render(struct cg_decor *d)
{
	struct wlr_xwayland_surface *xs = xsurface(d->view);
	int w = d->width;
	uint32_t bg = 0xffffffff, fg = d->active ? 0xff000000 : 0xff999999;
	uint32_t border = d->active ? 0xff707070 : 0xffaaaaaa;

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
		fill(p, w, w - 2 * BUTTON_W, 0, w - BUTTON_W, DECOR_TITLE_H, 0xffe5e5e5);
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

	draw_title(p, w, 12, w - 2 * BUTTON_W - 20, xs->title ? xs->title : "", fg);

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
	d->commit.notify = handle_commit;
	wl_signal_add(&view->wlr_surface->events.commit, &d->commit);
	d->set_title.notify = handle_set_title;
	wl_signal_add(&xs->events.set_title, &d->set_title);
	wl_list_insert(&decors, &d->link);
	view->decorated = true;
	render(d);
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
			*part = sx >= d->width - BUTTON_W ? PART_CLOSE : sx >= d->width - 2 * BUTTON_W ? PART_MAX : PART_BAR;
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
