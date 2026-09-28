/*
 * sg-compositor: the backdrop -- what shows where no window is.
 *
 * Stained Glass OS: the screen was black whenever nothing covered it -- after
 * the first-run setup while the first session was made, and at every sign-in
 * until the desktop was drawn -- for up to a minute, and a black screen reads
 * as a hang. Under every window there is now the backdrop: the colour and the
 * centred picture (the mark and "Getting things ready") of
 * /usr/share/stained-glass/backdrop.sgbd, which sg-shell draws. Without that
 * file, the colour alone.
 *
 * The file: "SGBD", width, height, colour (0xAARRGGBB), all little-endian
 * uint32, then width x height premultiplied ARGB8888 pixels.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

#include "backdrop.h"
#include "output.h"
#include "server.h"

#define BACKDROP_FILE "/usr/share/stained-glass/backdrop.sgbd"
#define BACKDROP_MAX 4096

struct pixels {
	struct wlr_buffer base;
	void *data;
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

static uint32_t
le32(const unsigned char *b)
{
	return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t) b[3] << 24;
}

/* the picture and colour, once */
static struct pixels *g_picture;
static float g_color[4] = {0x2a / 255.0f, 0x16 / 255.0f, 0x4c / 255.0f, 1.0f};
static bool g_loaded;

static void
load(void)
{
	const char *path = getenv("SG_BACKDROP");
	unsigned char head[16];
	uint32_t w, h, c;
	FILE *f;

	g_loaded = true;
	if (!(f = fopen(path && *path ? path : BACKDROP_FILE, "rb"))) {
		return;
	}
	if (fread(head, 1, sizeof(head), f) == sizeof(head) && !memcmp(head, "SGBD", 4)) {
		w = le32(head + 4);
		h = le32(head + 8);
		c = le32(head + 12);
		g_color[0] = ((c >> 16) & 0xff) / 255.0f;
		g_color[1] = ((c >> 8) & 0xff) / 255.0f;
		g_color[2] = (c & 0xff) / 255.0f;
		if (w && h && w <= BACKDROP_MAX && h <= BACKDROP_MAX) {
			struct pixels *p = calloc(1, sizeof(*p));
			size_t size = (size_t) w * h * 4;
			if (p && (p->data = malloc(size)) && fread(p->data, 1, size, f) == size) {
				p->stride = (size_t) w * 4;
				wlr_buffer_init(&p->base, &pixels_impl, w, h);
				g_picture = p;
			} else if (p) {
				free(p->data);
				free(p);
			}
		}
	}
	fclose(f);
}

void
backdrop_update(struct cg_server *server)
{
	struct wlr_scene_node *node, *tmp;
	struct cg_output *output;

	if (!server->backdrop_tree) {
		return;
	}
	if (!g_loaded) {
		load();
	}
	wl_list_for_each_safe (node, tmp, &server->backdrop_tree->children, link) {
		wlr_scene_node_destroy(node);
	}
	wl_list_for_each (output, &server->outputs, link) {
		struct wlr_box box;
		struct wlr_scene_rect *rect;

		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (wlr_box_empty(&box)) {
			continue;
		}
		if ((rect = wlr_scene_rect_create(server->backdrop_tree, box.width, box.height, g_color))) {
			wlr_scene_node_set_position(&rect->node, box.x, box.y);
		}
		if (g_picture && g_picture->base.width <= box.width && g_picture->base.height <= box.height) {
			struct wlr_scene_buffer *pic = wlr_scene_buffer_create(server->backdrop_tree, &g_picture->base);
			if (pic) {
				wlr_scene_node_set_position(&pic->node, box.x + (box.width - g_picture->base.width) / 2,
							    box.y + (box.height - g_picture->base.height) / 2);
			}
		}
	}
}

void
backdrop_init(struct cg_server *server)
{
	server->backdrop_tree = wlr_scene_tree_create(&server->scene->tree);
	if (server->backdrop_tree) {
		/* beneath every window */
		wlr_scene_node_lower_to_bottom(&server->backdrop_tree->node);
	}
}
