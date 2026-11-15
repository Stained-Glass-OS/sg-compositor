/*
 * sg-compositor: explicit synchronization (linux-drm-syncobj-v1) on
 * wlroots 0.18.
 *
 * NVIDIA's driver does no implicit synchronization of shared buffers. Without
 * the protocol, Xwayland's buffers were shown before its GPU had finished
 * drawing them and reused while still shown: Steam's pages scrambled, games
 * flickered (David's RTX 3090, 2026-11-15). With it, Xwayland (24.1 and
 * NVIDIA 555 or later) attaches an acquire point (the buffer is drawn) and a
 * release point (the compositor is done with it) to every buffer.
 *
 * wlroots 0.18 advertises the protocol and holds a commit until its acquire
 * fence exists; using the points is the compositor's (0.19 does it in the
 * renderer). Here:
 *  - acquire: before the scene takes a committed buffer (this listener is
 *    added at the surface's creation, ahead of the scene's), wait until its
 *    acquire point is signalled -- briefly: the client submitted the work
 *    already; after ACQUIRE_WAIT_MS it is shown anyway;
 *  - release: when the buffer is released (the scene and renderer let it
 *    go), its release point is signalled at the next output frame -- a frame
 *    event comes after the previous frame was presented, so nothing reads the
 *    buffer then -- or after RELEASE_FALLBACK_MS if no frame comes (an output
 *    turned off).
 *
 * Used with NVIDIA's driver only (nvidia-drm), where implicit synchronization
 * is missing; elsewhere it stays off unless SG_EXPLICIT_SYNC=1. SG_EXPLICIT_SYNC=0
 * turns it off everywhere.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

#include "syncobj.h"

#define ACQUIRE_WAIT_MS 100
#define RELEASE_FALLBACK_MS 50

struct sync_surface {
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* a release point to signal when its buffer is let go */
struct release {
	struct wl_list link; /* waiting (on the buffer) or due (at the next frame) */
	struct wlr_drm_syncobj_timeline *timeline;
	uint64_t point;
	struct wl_listener buffer_release;
	struct wl_listener buffer_destroy;
	bool due;
};

static struct {
	bool enabled;
	int drm_fd;
	struct wl_list releases;
	struct wl_listener new_surface;
	struct wl_event_source *fallback;
} sync;

static void signal_release(struct release *r)
{
	if (drmSyncobjTimelineSignal(r->timeline->drm_fd, &r->timeline->handle, &r->point, 1) != 0) {
		wlr_log(WLR_ERROR, "explicit sync: cannot signal a release point");
	}
	wl_list_remove(&r->link);
	wl_list_remove(&r->buffer_release.link);
	wl_list_remove(&r->buffer_destroy.link);
	wlr_drm_syncobj_timeline_unref(r->timeline);
	free(r);
}

static void signal_due(void)
{
	struct release *r, *tmp;
	wl_list_for_each_safe (r, tmp, &sync.releases, link) {
		if (r->due) {
			signal_release(r);
		}
	}
}

static int handle_fallback(void *data)
{
	signal_due();
	return 0;
}

static void make_due(struct release *r)
{
	if (r->due) {
		return;
	}
	r->due = true;
	wl_list_remove(&r->buffer_release.link);
	wl_list_init(&r->buffer_release.link);
	if (sync.fallback) {
		wl_event_source_timer_update(sync.fallback, RELEASE_FALLBACK_MS);
	}
}

static void handle_buffer_release(struct wl_listener *listener, void *data)
{
	struct release *r = wl_container_of(listener, r, buffer_release);
	make_due(r);
}

static void handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct release *r = wl_container_of(listener, r, buffer_destroy);
	wl_list_remove(&r->buffer_destroy.link);
	wl_list_init(&r->buffer_destroy.link);
	make_due(r);
}

static void wait_acquire(struct wlr_drm_syncobj_timeline *timeline, uint64_t point)
{
	bool done = false;
	struct timespec now;
	int64_t deadline;

	if (wlr_drm_syncobj_timeline_check(timeline, point, 0, &done) && done) {
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + ACQUIRE_WAIT_MS * 1000000LL;
	if (drmSyncobjTimelineWait(timeline->drm_fd, &timeline->handle, &point, 1, deadline, 0, NULL) != 0) {
		wlr_log(WLR_DEBUG, "explicit sync: a buffer's acquire point was not signalled in %d ms", ACQUIRE_WAIT_MS);
	}
}

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
	struct sync_surface *s = wl_container_of(listener, s, commit);
	struct wlr_linux_drm_syncobj_surface_v1_state *state = wlr_linux_drm_syncobj_v1_get_surface_state(s->surface);
	struct wlr_buffer *buffer = s->surface->current.buffer;

	if (!state || !(s->surface->current.committed & WLR_SURFACE_STATE_BUFFER) || !buffer) {
		return;
	}
	if (state->acquire_timeline) {
		wait_acquire(state->acquire_timeline, state->acquire_point);
	}
	if (state->release_timeline) {
		struct release *r = calloc(1, sizeof(*r));
		if (!r) {
			return;
		}
		r->timeline = wlr_drm_syncobj_timeline_ref(state->release_timeline);
		r->point = state->release_point;
		r->buffer_release.notify = handle_buffer_release;
		wl_signal_add(&buffer->events.release, &r->buffer_release);
		r->buffer_destroy.notify = handle_buffer_destroy;
		wl_signal_add(&buffer->events.destroy, &r->buffer_destroy);
		wl_list_insert(&sync.releases, &r->link);
	}
}

static void handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct sync_surface *s = wl_container_of(listener, s, destroy);
	wl_list_remove(&s->commit.link);
	wl_list_remove(&s->destroy.link);
	free(s);
}

static void handle_new_surface(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct sync_surface *s = calloc(1, sizeof(*s));
	if (!s) {
		return;
	}
	s->surface = surface;
	s->commit.notify = handle_surface_commit;
	wl_signal_add(&surface->events.commit, &s->commit);
	s->destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &s->destroy);
}

void cg_syncobj_init(struct wl_display *display, struct wlr_renderer *renderer, struct wlr_compositor *compositor)
{
	const char *env = getenv("SG_EXPLICIT_SYNC");
	int fd = wlr_renderer_get_drm_fd(renderer);
	uint64_t cap = 0;
	bool nvidia = false;

	if (fd < 0 || (env && !strcmp(env, "0"))) {
		return;
	}
	drmVersion *v = drmGetVersion(fd);
	if (v) {
		nvidia = v->name && !strcmp(v->name, "nvidia-drm");
		drmFreeVersion(v);
	}
	if (!nvidia && !(env && !strcmp(env, "1"))) {
		return;
	}
	if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || !cap) {
		wlr_log(WLR_INFO, "explicit sync: the GPU driver has no timeline sync objects (NVIDIA 555 or later has)");
		return;
	}
	if (!wlr_linux_drm_syncobj_manager_v1_create(display, 1, fd)) {
		wlr_log(WLR_ERROR, "explicit sync: cannot create linux-drm-syncobj-v1");
		return;
	}
	sync.enabled = true;
	sync.drm_fd = fd;
	wl_list_init(&sync.releases);
	sync.fallback = wl_event_loop_add_timer(wl_display_get_event_loop(display), handle_fallback, NULL);
	sync.new_surface.notify = handle_new_surface;
	wl_signal_add(&compositor->events.new_surface, &sync.new_surface);
	wlr_log(WLR_INFO, "explicit sync: linux-drm-syncobj-v1 offered (%s)", nvidia ? "NVIDIA's driver" : "SG_EXPLICIT_SYNC=1");
}

void cg_syncobj_frame(void)
{
	if (sync.enabled) {
		signal_due();
	}
}
