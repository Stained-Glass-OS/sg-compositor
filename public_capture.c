/*
 * sg-compositor: screen capture for everyone, with the secure screens left
 * out (David, 2026-09-30: "let any app capture, with lock and consent screens
 * blanked out of captures").
 *
 * wlroots' screencopy (wlr-screencopy-unstable-v1) stays for privileged
 * clients only -- the Remote Desktop daemon must see the lock screen. Every
 * other client gets this one, a second global of the same protocol: it copies
 * the output as it is shown, except while the session is locked, a secure
 * prompt (elevation consent) is up, or the security screen (Ctrl+Alt+Del) is
 * open -- then the frame is black. OBS and the portal's screen sharing
 * (xdg-desktop-portal-wlr) use it. Copies go to shared-memory buffers only;
 * frames are made from the buffer the output commits.
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include "public_capture.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "lock.h"
#include "server.h"
#include "wlr-screencopy-unstable-v1-protocol.h"

#define MANAGER_VERSION 3

struct frame {
	struct wl_resource *resource;
	struct cg_server *server;
	struct wlr_output *output;
	struct wlr_box box; /* in the output's buffer pixels */
	struct wlr_buffer *buffer; /* the client's, while a copy waits */
	bool with_damage;
	bool render_locked; /* no direct scanout while a copy waits: a client's buffer cannot be read */
	struct wl_listener commit;
	struct wl_listener output_destroy;
};

/* the session is showing something no one else may see */
static bool
secure_now(struct cg_server *server)
{
	return server->lock.locked || server->lock.secure || server->lock.sas;
}

static void
frame_stop(struct frame *f)
{
	if (f->buffer) {
		wlr_buffer_unlock(f->buffer);
		f->buffer = NULL;
	}
	if (f->commit.link.next) {
		wl_list_remove(&f->commit.link);
		f->commit.link.next = NULL;
	}
	if (f->render_locked && f->output) {
		wlr_output_lock_attach_render(f->output, false);
	}
	f->render_locked = false;
}

static void
frame_failed(struct frame *f)
{
	frame_stop(f);
	zwlr_screencopy_frame_v1_send_failed(f->resource);
}

static void
frame_unbind_output(struct frame *f)
{
	if (f->render_locked && f->output) {
		wlr_output_lock_attach_render(f->output, false);
		f->render_locked = false;
	}
	if (f->output_destroy.link.next) {
		wl_list_remove(&f->output_destroy.link);
		f->output_destroy.link.next = NULL;
	}
	f->output = NULL;
}

/* the client's buffer, filled from the output's: copied, or black */
static bool
fill(struct frame *f, struct wlr_buffer *src)
{
	void *data;
	uint32_t format;
	size_t stride;
	bool ok = true;

	if (!wlr_buffer_begin_data_ptr_access(f->buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return false;
	}
	if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
		ok = false;
	} else if (secure_now(f->server) || !src) {
		for (int y = 0; y < f->box.height; y++) {
			uint32_t *row = (uint32_t *) ((char *) data + (size_t) y * stride);
			for (int x = 0; x < f->box.width; x++) {
				row[x] = 0xff000000;
			}
		}
	} else {
		struct wlr_texture *tex = wlr_texture_from_buffer(f->output->renderer ? f->output->renderer : f->server->renderer, src);
		if (!tex) {
			wlr_log(WLR_ERROR, "public capture: no texture from the output's buffer");
			ok = false;
		} else {
			ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
								  .data = data,
								  .format = format,
								  .stride = stride,
								  .src_box = f->box,
							  });
			wlr_texture_destroy(tex);
		}
	}
	wlr_buffer_end_data_ptr_access(f->buffer);
	return ok;
}

static void
frame_handle_commit(struct wl_listener *listener, void *data)
{
	struct frame *f = wl_container_of(listener, f, commit);
	struct wlr_output_event_commit *ev = data;
	struct timespec now;

	if (!(ev->state->committed & WLR_OUTPUT_STATE_BUFFER) || !ev->state->buffer) {
		return; /* not a new picture: wait for one */
	}
	if (!fill(f, ev->state->buffer)) {
		wlr_log(WLR_ERROR, "public capture: copying %dx%d failed", f->box.width, f->box.height);
		frame_failed(f);
		return;
	}
	frame_stop(f);
	zwlr_screencopy_frame_v1_send_flags(f->resource, 0);
	if (f->with_damage) {
		zwlr_screencopy_frame_v1_send_damage(f->resource, 0, 0, f->box.width, f->box.height);
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	zwlr_screencopy_frame_v1_send_ready(f->resource, (uint32_t) ((uint64_t) now.tv_sec >> 32),
					    (uint32_t) now.tv_sec, (uint32_t) now.tv_nsec);
}

static void
frame_handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct frame *f = wl_container_of(listener, f, output_destroy);
	(void) data;
	frame_unbind_output(f);
	if (f->buffer) {
		frame_failed(f);
	}
}

static void
frame_copy(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer_resource,
	   bool with_damage)
{
	struct frame *f = wl_resource_get_user_data(resource);
	struct wlr_buffer *buffer;
	void *data;
	uint32_t format;
	size_t stride;
	(void) client;

	if (!f) {
		return;
	}
	if (f->buffer) {
		wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED, "frame already used");
		return;
	}
	if (!f->output || !(buffer = wlr_buffer_try_from_resource(buffer_resource))) {
		zwlr_screencopy_frame_v1_send_failed(resource);
		return;
	}
	if (buffer->width != f->box.width || buffer->height != f->box.height ||
	    !wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		wlr_buffer_unlock(buffer);
		wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer");
		return;
	}
	wlr_buffer_end_data_ptr_access(buffer);
	if ((format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) || stride < (size_t) f->box.width * 4) {
		wlr_buffer_unlock(buffer);
		wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER, "invalid buffer format");
		return;
	}
	f->buffer = buffer;
	f->with_damage = with_damage;
	f->commit.notify = frame_handle_commit;
	wl_signal_add(&f->output->events.commit, &f->commit);
	wlr_output_lock_attach_render(f->output, true);
	f->render_locked = true;
	wlr_output_update_needs_frame(f->output);
}

static void
frame_handle_copy(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer)
{
	frame_copy(client, resource, buffer, false);
}

static void
frame_handle_copy_with_damage(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer)
{
	frame_copy(client, resource, buffer, true);
}

static void
frame_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void) client;
	wl_resource_destroy(resource);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
	.copy = frame_handle_copy,
	.destroy = frame_handle_destroy,
	.copy_with_damage = frame_handle_copy_with_damage,
};

static void
frame_resource_destroy(struct wl_resource *resource)
{
	struct frame *f = wl_resource_get_user_data(resource);
	if (!f) {
		return;
	}
	frame_stop(f);
	frame_unbind_output(f);
	free(f);
}

static void
capture(struct wl_client *client, struct wl_resource *manager, uint32_t id, struct wl_resource *output_resource,
	const struct wlr_box *region)
{
	struct cg_server *server = wl_resource_get_user_data(manager);
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	struct wl_resource *resource =
		wl_resource_create(client, &zwlr_screencopy_frame_v1_interface, wl_resource_get_version(manager), id);
	struct frame *f;

	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	if (!output || !output->enabled || !(f = calloc(1, sizeof(*f)))) {
		wl_resource_set_implementation(resource, &frame_impl, NULL, frame_resource_destroy);
		zwlr_screencopy_frame_v1_send_failed(resource);
		return;
	}
	f->resource = resource;
	f->server = server;
	f->output = output;
	f->box = (struct wlr_box){0, 0, output->width, output->height};
	if (region) {
		/* logical coordinates, in the output's pixels */
		struct wlr_box r = {
			(int) (region->x * output->scale),
			(int) (region->y * output->scale),
			(int) (region->width * output->scale),
			(int) (region->height * output->scale),
		};
		if (!wlr_box_intersection(&f->box, &f->box, &r)) {
			f->box = (struct wlr_box){0, 0, 0, 0};
		}
	}
	f->output_destroy.notify = frame_handle_output_destroy;
	wl_signal_add(&output->events.destroy, &f->output_destroy);
	wl_resource_set_implementation(resource, &frame_impl, f, frame_resource_destroy);
	if (wlr_box_empty(&f->box)) {
		zwlr_screencopy_frame_v1_send_failed(resource);
		return;
	}
	zwlr_screencopy_frame_v1_send_buffer(resource, WL_SHM_FORMAT_XRGB8888, f->box.width, f->box.height,
					     f->box.width * 4);
	if (wl_resource_get_version(resource) >= 3) {
		zwlr_screencopy_frame_v1_send_buffer_done(resource);
	}
}

static void
manager_capture_output(struct wl_client *client, struct wl_resource *manager, uint32_t id, int32_t overlay_cursor,
		       struct wl_resource *output)
{
	(void) overlay_cursor;
	capture(client, manager, id, output, NULL);
}

static void
manager_capture_output_region(struct wl_client *client, struct wl_resource *manager, uint32_t id,
			      int32_t overlay_cursor, struct wl_resource *output, int32_t x, int32_t y, int32_t width,
			      int32_t height)
{
	struct wlr_box region = {x, y, width, height};
	(void) overlay_cursor;
	capture(client, manager, id, output, &region);
}

static void
manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void) client;
	wl_resource_destroy(resource);
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
	.capture_output = manager_capture_output,
	.capture_output_region = manager_capture_output_region,
	.destroy = manager_destroy,
};

static void
manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &zwlr_screencopy_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

const struct wl_global *
public_capture_create(struct cg_server *server)
{
	return wl_global_create(server->wl_display, &zwlr_screencopy_manager_v1_interface, MANAGER_VERSION, server,
				manager_bind);
}
