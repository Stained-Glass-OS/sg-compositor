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
 * The bar is the Wine frames' own: their look (Classic, Rounded, Horizon or
 * Glass) and sizes, which Settings writes to effects.conf with the window
 * style (frame=, caption= the title bar's height, button= the caption
 * buttons' width, border= the frame's thickness, scale8= the screen's
 * scale, glass= the Glass frame's opacity) -- a Linux window is put into a
 * Wine frame a moment after it maps, and the two looked different (David
 * 2026-10-02: "Linux apps seem to get smaller window decorations").
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
#include <time.h>
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
#define DEFAULT_BUTTON_W 46
#else
#define DEFAULT_BUTTON_W 30
#endif
#define ICON_PX 16   /* the program's icon at the bar's left, as Wine's windows have theirs */
#define DOUBLE_CLICK_MS 400

/* ---- the look: Settings' effects.conf ------------------------------------ */

enum look { LOOK_CLASSIC, LOOK_ROUNDED, LOOK_HORIZON, LOOK_GLASS };
static struct {
	enum look look;
	int caption;  /* the title bar's height (SM_CYCAPTION); 0: our own 32 px bar */
	int button;   /* a caption button's width (SM_CXSIZE) */
	int buttonh;  /* ...its height (SM_CYSIZE) */
	int border;   /* the frame's thickness, its outer line included */
	int scale8;   /* the screen's scale, in eighths */
	int glass;    /* the Glass frame's opacity in percent (0: opaque) */
	int taskbar;  /* the shell's taskbar's height (0: 40 px) */
	int cursor;   /* the pointer's size (0: the screen's own) */
} conf = {LOOK_CLASSIC, 0, DEFAULT_BUTTON_W, 0, 1, 8, 0, 0, 0};

static void
read_conf(void)
{
	static struct timespec seen;
	static bool had;
	char path[512], line[256];
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	struct stat st;
	FILE *f;

	if (cfg && *cfg) {
		snprintf(path, sizeof(path), "%s/stained-glass/effects.conf", cfg);
	} else if (home) {
		snprintf(path, sizeof(path), "%s/.config/stained-glass/effects.conf", home);
	} else {
		return;
	}
	if (stat(path, &st) != 0) {
		if (had) {
			conf.look = LOOK_CLASSIC; conf.caption = 0; conf.button = DEFAULT_BUTTON_W;
			conf.buttonh = 0; conf.border = 1; conf.scale8 = 8; conf.glass = 0;
			conf.taskbar = 0; conf.cursor = 0;
		}
		had = false;
		return;
	}
	if (had && st.st_mtim.tv_sec == seen.tv_sec && st.st_mtim.tv_nsec == seen.tv_nsec) {
		return;
	}
	seen = st.st_mtim;
	had = true;
	conf.look = LOOK_CLASSIC; conf.caption = 0; conf.button = DEFAULT_BUTTON_W;
	conf.buttonh = 0; conf.border = 1; conf.scale8 = 8; conf.glass = 0;
	conf.taskbar = 0; conf.cursor = 0;
	if (!(f = fopen(path, "r"))) {
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');
		int n;
		if (!v) {
			continue;
		}
		*v++ = 0;
		n = atoi(v);
		if (!strcmp(line, "frame")) {
			conf.look = !strncmp(v, "rounded", 7) ? LOOK_ROUNDED : !strncmp(v, "horizon", 7) ? LOOK_HORIZON
				  : !strncmp(v, "glass", 5)   ? LOOK_GLASS   : LOOK_CLASSIC;
		} else if (!strcmp(line, "caption") && n >= 12 && n <= 120) {
			conf.caption = n;
		} else if (!strcmp(line, "button") && n >= 12 && n <= 160) {
			conf.button = n;
		} else if (!strcmp(line, "buttonh") && n >= 10 && n <= 120) {
			conf.buttonh = n;
		} else if (!strcmp(line, "border") && n >= 1 && n <= 30) {
			conf.border = n;
		} else if (!strcmp(line, "scale8") && n >= 8 && n <= 24) {
			conf.scale8 = n;
		} else if (!strcmp(line, "glass") && n >= 0 && n < 100) {
			conf.glass = n;
		} else if (!strcmp(line, "taskbar") && n >= 20 && n <= 400) {
			conf.taskbar = n;
		} else if (!strcmp(line, "cursor") && n >= 16 && n <= 256) {
			conf.cursor = n;
		}
	}
	fclose(f);
#ifdef SG_MUTANT_DECOR_OWN_SIZE
	conf.look = LOOK_CLASSIC; conf.caption = 0; conf.button = DEFAULT_BUTTON_W; conf.border = 1; conf.scale8 = 8;
#endif
}

/* the bar's height: the Wine frame's top -- its frame inside the outer line,
 * and the caption -- or our own 32 px */
int
decor_title_h(void)
{
	read_conf();
	return conf.caption ? conf.border - 1 + conf.caption : 32;
}

/* the taskbar's height at the display scale (David 2026-10-05: on a
 * 2736x1824 screen at 175% it is 70 px; a maximized Linux window stopped 40
 * px from the bottom, under it) */
int
decor_taskbar_h(void)
{
	read_conf();
#ifndef SG_MUTANT_TASKBAR_FIXED
	return conf.taskbar ? conf.taskbar : 40;
#else
	return 40;
#endif
}

int
decor_cursor_size(void)
{
	read_conf();
	return conf.cursor;
}

static int button_w(void) { return conf.button; }
/* the frame's body inside its one-pixel outer line, at the sides and bottom */
static int body_w(void) { return conf.caption ? conf.border - 1 : 0; }
static int px8(int px) { return (px * conf.scale8 + 4) / 8; }
#define DECOR_TITLE_H bar_h
static int bar_h = 32;   /* the bar being drawn's height */

enum part { PART_NONE, PART_BAR, PART_MIN, PART_MAX, PART_CLOSE };

struct cg_decor {
	struct wl_list link; /* decors */
	struct cg_view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *bar;
	struct wlr_scene_rect *border[4];
	struct wlr_scene_rect *body[3];   /* the frame's body: left, right, bottom */
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

static int font_px, font_bold;

static void
font_size(int px, bool bold)
{
	if (font_px != px) {
		FT_Set_Pixel_Sizes(face, 0, px);
		font_px = px;
	}
	font_bold = bold;
}

static int
text_width(const char *s, int n)
{
	int w = 0;
	const char *end = s + n;
	while (s < end && *s) {
		if (!FT_Load_Char(face, utf8_next(&s), FT_LOAD_DEFAULT)) {
			w += (face->glyph->advance.x >> 6) + (font_bold ? 1 : 0);
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
				if (font_bold) {   /* emboldened: once more a pixel over */
					blend(p, w, x + 1 + g->bitmap_left + (int) c, baseline - g->bitmap_top + (int) r, color,
					      g->bitmap.buffer[r * g->bitmap.pitch + c]);
				}
			}
		}
		x += (g->advance.x >> 6) + (font_bold ? 1 : 0);
	}
	return x;
}

/* the title in the room there is, with "..." when it does not fit */
static void
draw_title(struct pixels *p, int w, int x, int room, const char *title, uint32_t color, int top)
{
	int n = strlen(title), dots;
	if (!title[0] || room <= 0 || !font_ready()) {
		return;
	}
	font_size(px8(12), conf.look == LOOK_HORIZON);
	/* in the middle of the caption (below the frame's top) */
	int baseline = top + (DECOR_TITLE_H - top) / 2 + px8(12) / 3;
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

/* a look's colours: the title bar's gradient (stops in thousandths of its
 * height), the frame's body and outer line, the title, the caption buttons'
 * gradients and glyphs -- as wine-sg's frames (win32u defwnd.c sg_frames) */
struct stop { int pos; uint32_t c; };
struct style {
	struct stop bar[6];
	uint32_t body, outer, text;
	struct stop btn[4], close[4];
	uint32_t glyph, close_glyph;
};
static const struct style horizon[2] = {
	{ {{0,0xa6bff5},{70,0x94b1f1},{250,0x86a5ec},{700,0x7d9de9},{900,0x7898e6},{1000,0x6c8cdc}},
	  0x7d9de9, 0x5f7fd4, 0xe2eafb,
	  {{0,0xb3c9f6},{500,0x9ab6f1},{1000,0x86a5ec},{1000,0x86a5ec}}, {{0,0xeeb8a6},{500,0xe29a82},{1000,0xd27e64},{1000,0xd27e64}},
	  0xf0f4fd, 0xfbf2ef },
	{ {{0,0x5d9dfb},{70,0x2c74f1},{250,0x165de6},{700,0x0c50dc},{900,0x0a4ad3},{1000,0x053ab4}},
	  0x0b4cd6, 0x00269a, 0xffffff,
	  {{0,0x6ca3fa},{500,0x3a77ec},{1000,0x225fdc},{1000,0x225fdc}}, {{0,0xef8d6e},{500,0xe05f3a},{1000,0xc23c18},{1000,0xc23c18}},
	  0xffffff, 0xffffff },
};
static const struct style glass[2] = {
	{ {{0,0xecf2fa},{80,0xe1eaf5},{500,0xd6e2f1},{1000,0xcfddef},{1000,0xcfddef},{1000,0xcfddef}},
	  0xd3e0f0, 0x8ca2bd, 0x555f6b,
	  {{0,0xf8fafd},{480,0xecf1f8},{520,0xdce5f1},{1000,0xe6edf6}}, {{0,0xf4e0dd},{480,0xeacac5},{520,0xdcada6},{1000,0xe4bfb9}},
	  0x6c7b8f, 0xfbf4f3 },
	{ {{0,0xd9e7f6},{80,0xc3d7ef},{500,0xadc8e8},{1000,0xa0bfe4},{1000,0xa0bfe4},{1000,0xa0bfe4}},
	  0xa7c3e6, 0x46668e, 0x000000,
	  {{0,0xf6f9fd},{480,0xe1eaf5},{520,0xc5d5ea},{1000,0xd7e4f3}}, {{0,0xf1b9ae},{480,0xe28d7c},{520,0xc84834},{1000,0xd97462}},
	  0x1f3352, 0xffffff },
};

static uint32_t
gradient(const struct stop *st, int n, int y, int h)
{
	int pos = h > 1 ? y * 1000 / (h - 1) : 0;
	for (int i = 0; i < n - 1; i++) {
		if (pos <= st[i + 1].pos && st[i + 1].pos > st[i].pos) {
			int span = st[i + 1].pos - st[i].pos, t = pos - st[i].pos;
			uint32_t a = st[i].c, b = st[i + 1].c, out = 0xff000000;
			if (t < 0) t = 0;
			for (int s = 0; s < 24; s += 8) {
				int ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
				out |= (uint32_t) (ca + (cb - ca) * t / span) << s;
			}
			return out;
		}
	}
	return 0xff000000 | st[n - 1].c;
}

/* a caption button of the Horizon and Glass looks: a rounded gradient */
static void
draw_era_button(struct pixels *p, int w, int x0, int y0, int bw, int bh, const struct stop *st, uint32_t edge, bool pressed)
{
	int r = px8(conf.look == LOOK_HORIZON ? 3 : 4);
	for (int y = 0; y < bh; y++) {
		uint32_t c = gradient(st, 4, y, bh);
		if (pressed) {
			c = 0xff000000 | (((c >> 16) & 0xff) * 3 / 4) << 16 | (((c >> 8) & 0xff) * 3 / 4) << 8 | ((c & 0xff) * 3 / 4);
		}
		for (int x = 0; x < bw; x++) {
			int dx = x < r ? r - x : x >= bw - r ? x - (bw - r - 1) : 0;
			int dy = y < r ? r - y : y >= bh - r ? y - (bh - r - 1) : 0;
			if (dx * dx + dy * dy > r * r) {
				continue;
			}
			bool rim = dx * dx + dy * dy > (r - 1) * (r - 1) || x == 0 || y == 0 || x == bw - 1 || y == bh - 1;
			put(p, w, x0 + x, y0 + y, rim && (dx || dy || x == 0 || y == 0 || x == bw - 1 || y == bh - 1) ? 0xff000000 | edge : c);
		}
	}
}

/* a glyph: a cross, a box, two boxes, a line; in a g x g square at (x, y) */
static void
draw_glyph(struct pixels *p, int w, enum part part, bool maximized, int x, int y, int g, int lw, uint32_t c)
{
	if (part == PART_CLOSE) {
		for (int i = 0; i <= g; i++) {
			for (int k = 0; k < lw; k++) {
				put(p, w, x + i + k, y + i, c);
				put(p, w, x + g - i + k, y + i, c);
			}
		}
	} else if (part == PART_MAX && !maximized) {
		for (int i = 0; i <= g; i++) {
			put(p, w, x + i, y + g, c);
			put(p, w, x, y + i, c);
			put(p, w, x + g, y + i, c);
			for (int k = 0; k < lw; k++) put(p, w, x + i, y + k, c);
		}
	} else if (part == PART_MAX) {
		int dd = g / 3, s = g - dd;
		for (int i = 0; i <= s; i++) {
			put(p, w, x + dd + i, y, c);         /* the back one: its top and right */
			put(p, w, x + g, y + i, c);
			put(p, w, x + i, y + dd, c);         /* the front one */
			put(p, w, x + i, y + g, c);
			put(p, w, x, y + dd + i, c);
			put(p, w, x + s, y + dd + i, c);
		}
	} else if (part == PART_MIN) {
		for (int i = 0; i <= g * 2 / 3; i++) {
			for (int k = 0; k < lw; k++) put(p, w, x + i, y + g - k, c);
		}
	}
}

static void
render(struct cg_decor *d)
{
	struct wlr_xwayland_surface *xs = xsurface(d->view);
	read_conf();
	/* the bar spans the frame: the window and the frame's body at its sides */
	int w = d->width + 2 * body_w();
	bool dark = dark_mode();
	bool era = conf.look == LOOK_HORIZON || conf.look == LOOK_GLASS;
	const struct style *st = conf.look == LOOK_HORIZON ? &horizon[d->active] : &glass[d->active];
	uint32_t bg = dark ? 0xff202020 : 0xffffffff;
	uint32_t fg = dark ? (d->active ? 0xffffffff : 0xff8a8a8a) : (d->active ? 0xff000000 : 0xff999999);
	uint32_t hover = dark ? 0xff3a3a3a : 0xffe5e5e5;
#ifdef SG_MUTANT_WIDE_BUTTONS
	uint32_t border = d->active ? 0xff707070 : 0xffaaaaaa;
#else
	uint32_t border = dark ? 0xff404040 : 0xffe3e3e3;
#endif
	uint32_t body = bg;
	int bw = button_w(), top = body_w(), round = conf.look == LOOK_ROUNDED ? px8(8) : era ? px8(conf.look == LOOK_HORIZON ? 7 : 6) : 0;
	/* the Glass frame see-through, as sg-deskcomp draws Wine's (no blur here) */
	uint32_t alpha = conf.look == LOOK_GLASS && conf.glass ? (uint32_t) conf.glass * 255 / 100 : 255;

	if (d->width < 1) {
		return;
	}
	bar_h = decor_title_h();
	if (era) {
		fg = 0xff000000 | st->text;
		border = 0xff000000 | st->outer;
		body = 0xff000000 | st->body;
	}
	struct pixels *p = calloc(1, sizeof(*p));
	if (!p || !(p->data = malloc((size_t) w * DECOR_TITLE_H * 4))) {
		free(p);
		return;
	}
	p->stride = (size_t) w * 4;
	if (era) {
		/* the gradient across the whole bar, the frame's top in it too */
		for (int y = 0; y < DECOR_TITLE_H; y++) {
			fill(p, w, 0, y, w, y + 1, gradient(st->bar, 6, y, DECOR_TITLE_H));
		}
	} else {
		fill(p, w, 0, 0, w, DECOR_TITLE_H, bg);
	}

	/* the caption buttons: Wine's slots (button wide, from the right inside
	 * the frame), drawn as Wine draws them */
	int right = w - top - (conf.caption ? 2 : 0);
	int slot_h = conf.caption ? (conf.buttonh ? conf.buttonh : conf.caption) - 4 : DECOR_TITLE_H;
	int slot_y = conf.caption ? (DECOR_TITLE_H - slot_h) / 2 : 0;
	enum part parts[3] = {PART_CLOSE, PART_MAX, PART_MIN};
	for (int i = 0; i < 3; i++) {
		int x1 = right - i * bw, x0 = x1 - bw + (conf.caption ? 2 : 0);
		int g = (era ? (x1 - x0 < slot_h ? x1 - x0 : slot_h) * 9 / 20 : px8(10));
		uint32_t c = fg;
		if (era) {
			int sw = x1 - x0, sx = x0;
			if (sw > slot_h + 2 && parts[i] != PART_CLOSE) {
				sx += (sw - slot_h) / 2;
				sw = slot_h;
			}
			draw_era_button(p, w, sx, slot_y, sw, slot_h, parts[i] == PART_CLOSE ? st->close : st->btn,
					parts[i] == PART_CLOSE ? 0x6a241c : st->outer, d->hover == parts[i]);
			c = 0xff000000 | (parts[i] == PART_CLOSE ? st->close_glyph : st->glyph);
			draw_glyph(p, w, parts[i], d->view->maximized, sx + (sw - g) / 2, slot_y + (slot_h - g) / 2, g,
				   px8(2), c);
			continue;
		}
		if (d->hover == parts[i]) {
			fill(p, w, x0, slot_y, x1, slot_y + slot_h, parts[i] == PART_CLOSE ? 0xffe81123 : hover);
			if (parts[i] == PART_CLOSE) {
				c = 0xffffffff;
			}
		}
		draw_glyph(p, w, parts[i], d->view->maximized, (x0 + x1 - g) / 2, slot_y + (slot_h - g) / 2, g, 1, c);
	}

	fetch_icon(d, xs);
	int icon_x = top + px8(7), title_x = icon_x + ICON_PX + px8(3);
	if (d->has_icon) {
		/* over the bar: premultiplied over */
		int iy = top + (DECOR_TITLE_H - top - ICON_PX) / 2;
		for (int y = 0; y < ICON_PX; y++) {
			for (int x = 0; x < ICON_PX && icon_x + x < w; x++) {
				uint32_t s = d->icon[y * ICON_PX + x], a = s >> 24, b = p->data[(iy + y) * w + icon_x + x];
				uint32_t r = ((s >> 16) & 0xff) + ((b >> 16) & 0xff) * (255 - a) / 255;
				uint32_t g = ((s >> 8) & 0xff) + ((b >> 8) & 0xff) * (255 - a) / 255;
				uint32_t bl = (s & 0xff) + (b & 0xff) * (255 - a) / 255;
				p->data[(iy + y) * w + icon_x + x] = 0xff000000 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (bl > 255 ? 255 : bl);
			}
		}
	}
	int tx = d->has_icon ? title_x : top + 12;
	draw_title(p, w, tx, right - 3 * bw - 8 - tx, xs->title ? xs->title : "", fg, top);

	/* round top corners (Rounded, Horizon, Glass), and Glass see-through */
	for (int y = 0; y < DECOR_TITLE_H; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t a = alpha;
			if (round && y < round) {
				int dx = x < round ? round - x : x >= w - round ? x - (w - round - 1) : 0, dy = round - y;
				if (dx && dx * dx + dy * dy > round * round) {
					a = 0;
				}
			}
			if (a != 255) {
				uint32_t c = p->data[y * w + x];   /* premultiplied */
				p->data[y * w + x] = a << 24 | (((c >> 16) & 0xff) * a / 255) << 16 | (((c >> 8) & 0xff) * a / 255) << 8 | ((c & 0xff) * a / 255);
			}
		}
	}

	wlr_buffer_init(&p->base, &pixels_impl, w, DECOR_TITLE_H);
	wlr_scene_buffer_set_buffer(d->bar, &p->base);
	wlr_buffer_drop(&p->base);

	/* the frame: its body at the sides and bottom (as thick as the Wine
	 * frame's), then a one-pixel outer line round it all */
	int h = d->height, b = body_w(), top_round = round ? round / 2 : 0;
	w = d->width;
	float cb[4] = {((body >> 16) & 0xff) / 255.0f * alpha / 255, ((body >> 8) & 0xff) / 255.0f * alpha / 255,
		       (body & 0xff) / 255.0f * alpha / 255, alpha / 255.0f};
	float c[4] = {((border >> 16) & 0xff) / 255.0f, ((border >> 8) & 0xff) / 255.0f, (border & 0xff) / 255.0f, 1};
	wlr_scene_rect_set_size(d->body[0], b, h + b);            /* left */
	wlr_scene_node_set_position(&d->body[0]->node, -b, 0);
	wlr_scene_rect_set_size(d->body[1], b, h + b);            /* right */
	wlr_scene_node_set_position(&d->body[1]->node, w, 0);
	wlr_scene_rect_set_size(d->body[2], w, b);                /* bottom */
	wlr_scene_node_set_position(&d->body[2]->node, 0, h);
	for (int i = 0; i < 3; i++) {
		wlr_scene_rect_set_color(d->body[i], cb);
		wlr_scene_node_set_enabled(&d->body[i]->node, b > 0);
	}
	wlr_scene_node_set_position(&d->bar->node, -b, -DECOR_TITLE_H);
	wlr_scene_rect_set_size(d->border[0], w + 2 + 2 * b - 2 * top_round, 1);
	wlr_scene_node_set_position(&d->border[0]->node, -1 - b + top_round, -DECOR_TITLE_H - 1);
	wlr_scene_rect_set_size(d->border[1], w + 2 + 2 * b, 1);
	wlr_scene_node_set_position(&d->border[1]->node, -1 - b, h + b);
	wlr_scene_rect_set_size(d->border[2], 1, h + b + DECOR_TITLE_H - top_round);
	wlr_scene_node_set_position(&d->border[2]->node, -1 - b, -DECOR_TITLE_H + top_round);
	wlr_scene_rect_set_size(d->border[3], 1, h + b + DECOR_TITLE_H - top_round);
	wlr_scene_node_set_position(&d->border[3]->node, w + b, -DECOR_TITLE_H + top_round);
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
	wlr_scene_node_set_position(&d->bar->node, 0, -decor_title_h());
	for (int i = 0; i < 4; i++) {
		d->border[i] = wlr_scene_rect_create(d->tree, 1, 1, (float[4]) {0.5f, 0.5f, 0.5f, 1});
	}
	for (int i = 0; i < 3; i++) {
		d->body[i] = wlr_scene_rect_create(d->tree, 0, 0, (float[4]) {1, 1, 1, 1});
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
			/* the buttons' columns, as render() puts them */
			int bw = button_w(), right = d->width + body_w() - (conf.caption ? 2 : 0);
			*part = sx >= right ? PART_BAR
				: sx >= right - bw     ? PART_CLOSE
				: sx >= right - 2 * bw ? PART_MAX
				: sx >= right - 3 * bw ? PART_MIN
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
		move_view(view, box.x + body_w(), box.y + decor_title_h(), box.width - 2 * body_w(), box.height - decor_title_h() - body_w() - decor_taskbar_h());
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
		if (y < box.y + decor_title_h()) {
			y = box.y + decor_title_h();
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
