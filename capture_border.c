/*
 * sg-compositor: the capture border.
 *
 * Windows draws a thin yellow frame round a window or monitor while
 * Windows.Graphics.Capture captures it, so the person knows; the frame is not
 * in the capture. Here a program asks for it on the control socket --
 * CAPTUREBORDER, anyone in the session, as INHIBIT and MAGNIFY -- and then
 * says where it is, a line at a time:
 *
 *   RECT x y w h    the captured window or monitor, in the Wine desktop's
 *                   coordinates (the layout's: its top left is 0,0); again
 *                   whenever it moves
 *   OFF             no frame for now
 *
 * The frame lasts as long as the connection: a program that ends, or ends
 * the capture, takes it with it. It is drawn above every window, below a
 * lock screen or secure prompt -- and not at all while one is up (the
 * capture is black then anyway). Captures never see it: public_capture.c
 * renders the output once more without the frames when one would be in the
 * picture (capture_border_render_without).
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include "capture_border.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "server.h"

#define MAX_BORDERS 16
#define THICKNESS 2

struct cg_capture_border {
	struct cg_server *server;
	int fd;
	struct wl_event_source *source;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *edge[4];
	struct wlr_box box; /* layout coordinates */
	bool shown;
	char line[128];
	size_t len;
	struct wl_list link;
};

static bool
secure_now(struct cg_server *server)
{
	return server->lock.locked || server->lock.secure || server->lock.sas;
}

static void
border_place(struct cg_capture_border *b)
{
	const int t = THICKNESS;
	struct wlr_box *r = &b->box;
	bool on = b->shown && r->width > 2 * t && r->height > 2 * t;

	if (on) {
		/* on the window's or monitor's own edge, as Windows draws it */
		wlr_scene_node_set_position(&b->edge[0]->node, r->x, r->y);
		wlr_scene_rect_set_size(b->edge[0], r->width, t);
		wlr_scene_node_set_position(&b->edge[1]->node, r->x, r->y + r->height - t);
		wlr_scene_rect_set_size(b->edge[1], r->width, t);
		wlr_scene_node_set_position(&b->edge[2]->node, r->x, r->y + t);
		wlr_scene_rect_set_size(b->edge[2], t, r->height - 2 * t);
		wlr_scene_node_set_position(&b->edge[3]->node, r->x + r->width - t, r->y + t);
		wlr_scene_rect_set_size(b->edge[3], t, r->height - 2 * t);
	}
	wlr_scene_node_set_enabled(&b->tree->node, on);
}

static void
border_free(struct cg_capture_border *b)
{
	if (b->source) {
		wl_event_source_remove(b->source);
	}
	if (b->fd >= 0) {
		close(b->fd);
	}
	if (b->tree) {
		wlr_scene_node_destroy(&b->tree->node);
	}
	wl_list_remove(&b->link);
	b->server->n_capture_borders--;
	free(b);
}

static bool
border_command(struct cg_capture_border *b, const char *line)
{
	int x, y, w, h;
	char extra;

	if (sscanf(line, "RECT %d %d %d %d %c", &x, &y, &w, &h, &extra) == 4 && w > 0 && h > 0 && w <= 65536 &&
	    h <= 65536) {
		b->box = (struct wlr_box){x, y, w, h};
		b->shown = true;
		border_place(b);
		return true;
	}
	if (!strcmp(line, "OFF")) {
		b->shown = false;
		border_place(b);
		return true;
	}
	return false;
}

static int
handle_readable(int fd, uint32_t mask, void *data)
{
	struct cg_capture_border *b = data;
	ssize_t n;
	char *nl;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
#ifdef SG_MUTANT_BORDER_STAYS
		wl_event_source_remove(b->source);
		b->source = NULL;
#else
		border_free(b);
#endif
		return 0;
	}
	n = recv(fd, b->line + b->len, sizeof(b->line) - 1 - b->len, 0);
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
		return 0;
	}
	if (n <= 0) {
#ifdef SG_MUTANT_BORDER_STAYS
		wl_event_source_remove(b->source);
		b->source = NULL;
#else
		border_free(b);
#endif
		return 0;
	}
	b->len += n;
	b->line[b->len] = 0;
	while ((nl = memchr(b->line, '\n', b->len))) {
		size_t used = nl + 1 - b->line;
		const char *reply;

		*nl = 0;
		reply = border_command(b, b->line) ? "OK\n" : "ERR\n";
		memmove(b->line, nl + 1, b->len - used);
		b->len -= used;
		b->line[b->len] = 0;
		if (send(fd, reply, strlen(reply), MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
			border_free(b);
			return 0;
		}
	}
	if (b->len == sizeof(b->line) - 1) {
		border_free(b); /* a line that never ends */
	}
	return 0;
}

void
capture_border_take_connection(struct cg_server *server, int fd)
{
	struct cg_capture_border *b;

	if (!server->capture_border_tree || server->n_capture_borders >= MAX_BORDERS ||
	    !(b = calloc(1, sizeof(*b)))) {
		send(fd, "ERR busy\n", 9, MSG_NOSIGNAL);
		close(fd);
		return;
	}
	b->server = server;
	b->fd = fd;
	wl_list_insert(&server->capture_borders, &b->link);
	server->n_capture_borders++;
	if (!(b->tree = wlr_scene_tree_create(server->capture_border_tree))) {
		border_free(b);
		return;
	}
	for (int i = 0; i < 4; i++) {
		/* Windows' capture border yellow */
		b->edge[i] = wlr_scene_rect_create(b->tree, 1, 1, (float[4]) {1.0f, 216.0f / 255.0f, 0.0f, 1.0f});
		if (!b->edge[i]) {
			border_free(b);
			return;
		}
	}
	wlr_scene_node_set_enabled(&b->tree->node, false);
	b->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), fd, WL_EVENT_READABLE,
					 handle_readable, b);
	if (send(fd, "OK capture border\n", 18, MSG_NOSIGNAL) < 0) {
		border_free(b);
	}
}

void
capture_border_update_visibility(struct cg_server *server)
{
	if (server->capture_border_tree) {
		wlr_scene_node_set_enabled(&server->capture_border_tree->node, !secure_now(server));
	}
}

bool
capture_border_in(struct cg_server *server, struct wlr_output *output, const struct wlr_box *box)
{
	struct cg_capture_border *b;
	struct wlr_box obox, r, hit;

	if (!server->capture_border_tree || !server->capture_border_tree->node.enabled) {
		return false;
	}
	wlr_output_layout_get_box(server->output_layout, output, &obox);
	wl_list_for_each (b, &server->capture_borders, link) {
		if (!b->tree->node.enabled) {
			continue;
		}
		/* in the output's buffer pixels */
		r.x = (int) ((b->box.x - obox.x) * output->scale);
		r.y = (int) ((b->box.y - obox.y) * output->scale);
		r.width = (int) (b->box.width * output->scale + 1);
		r.height = (int) (b->box.height * output->scale + 1);
		if (wlr_box_intersection(&hit, &r, box)) {
			return true;
		}
	}
	return false;
}

struct wlr_buffer *
capture_border_render_without(struct cg_server *server, struct wlr_output *output)
{
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server->scene, output);
	struct wlr_output_state state;
	struct wlr_buffer *buffer = NULL;

	if (!scene_output || !output->swapchain || !output->allocator) {
		return NULL;
	}
	if (server->capture_border_swapchain &&
	    (server->capture_border_swapchain->width != output->width ||
	     server->capture_border_swapchain->height != output->height ||
	     server->capture_border_swapchain->format.format != output->swapchain->format.format)) {
		wlr_swapchain_destroy(server->capture_border_swapchain);
		server->capture_border_swapchain = NULL;
	}
	if (!server->capture_border_swapchain &&
	    !(server->capture_border_swapchain = wlr_swapchain_create(output->allocator, output->width, output->height,
								      &output->swapchain->format))) {
		return NULL;
	}

	/* the scene once more, the frames left out; the output's own damage is
	 * only cleared when it commits, so its next picture is not affected */
	wlr_scene_node_set_enabled(&server->capture_border_tree->node, false);
	wlr_output_state_init(&state);
	if (wlr_scene_output_build_state(scene_output, &state,
					 &(struct wlr_scene_output_state_options){
						 .swapchain = server->capture_border_swapchain,
					 }) &&
	    (state.committed & WLR_OUTPUT_STATE_BUFFER) && state.buffer) {
		buffer = wlr_buffer_lock(state.buffer);
	}
	wlr_output_state_finish(&state);
	capture_border_update_visibility(server);
	return buffer;
}

void
capture_border_init(struct cg_server *server)
{
	wl_list_init(&server->capture_borders);
	server->n_capture_borders = 0;
	server->capture_border_swapchain = NULL;
}

void
capture_border_fini(struct cg_server *server)
{
	struct cg_capture_border *b, *tmp;
	wl_list_for_each_safe (b, tmp, &server->capture_borders, link) {
		border_free(b);
	}
	if (server->capture_border_swapchain) {
		wlr_swapchain_destroy(server->capture_border_swapchain);
		server->capture_border_swapchain = NULL;
	}
}
