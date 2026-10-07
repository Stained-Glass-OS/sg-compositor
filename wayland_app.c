/*
 * sg-compositor: a user program's native Wayland window, managed like the
 * session's other windows.
 *
 * Linux programs are meant to draw through Xwayland (sg-session tells GTK,
 * Qt, SDL, Firefox and Electron so), where Wine's taskbar frames them and
 * they stack, minimize and switch like Wine's own windows. A program that
 * still opens a native Wayland window -- Electron 39 and later decide by
 * XDG_SESSION_TYPE alone, and wlroots sets it to "wayland" -- got cage's
 * treatment: maximized over the whole screen, taskbar included, with no
 * taskbar button, no Alt+Tab entry, not movable, and once the desktop came
 * in front of it (Alt+Tab) no way back but killing it (David 2026-10-06,
 * the Claude desktop app).
 *
 * Here such a window -- an xdg_toplevel of an ordinary client (not the lock
 * screen's or a consent prompt's privileged connection) -- is a window:
 *   - placed in the work area (the screen less the taskbar), centred at the
 *     size it asks for, or maximized there when larger or when it asks;
 *   - never drawn over the taskbar strip (its surface is clipped there),
 *     unless it goes full screen, as on Windows;
 *   - moved and resized when it asks (its own title bar: xdg_toplevel.move
 *     and .resize), maximized, restored, minimized, made full screen;
 *   - on the taskbar, in Alt+Tab and Task View: listed by the control
 *     socket's XWINDOWS beside the X11 programs' windows, under an id no X
 *     window can have (bit 31 set: X ids are 29 bits), and XACTIVATE,
 *     XMINIMIZE, XCLOSE and XKILL act on it (session_x11.c). Wine cannot
 *     put it into a frame of its own, so the taskbar keeps its hidden
 *     stand-in, as for any window it cannot frame; XDESKTOP brings the
 *     desktop in front of it as of the X11 windows.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include "wayland_app.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "decor.h"
#include "elevated.h"
#include "lock.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "xdg_shell.h"

#define MIN_W 120
#define MIN_H 60

static struct cg_xdg_shell_view *
xv_of(struct cg_view *view)
{
	return (struct cg_xdg_shell_view *) view;
}

bool
wayland_app_is(struct cg_view *view)
{
#ifdef SG_MUTANT_WAYLAND_UNMANAGED
	return false;
#endif
	return view && view->type == CAGE_XDG_SHELL_VIEW && xv_of(view)->managed;
}

/* the screen less the taskbar: where a window lives */
static void
work_area(struct cg_view *view, struct wlr_box *box)
{
	view_get_layout_box(view, box);
	box->height -= DECOR_TASKBAR_H;
	if (box->height < MIN_H) {
		box->height = MIN_H;
	}
}

static struct wlr_box
geometry(struct cg_view *view)
{
	struct wlr_box geom;
	wlr_xdg_surface_get_geometry(xv_of(view)->xdg_toplevel->base, &geom);
	return geom;
}

/* Nothing of a window (but full screen) over the taskbar strip, as the
 * taskbar is always in front on Windows: its surface is cut off there.
 * Its menus (popups) are not, as Windows' are not either. */
static void
update_clip(struct cg_view *view)
{
	if (!view->scene_tree) {
		return;
	}
#ifndef SG_MUTANT_WAYLAND_OVER_TASKBAR
	if (!xv_of(view)->fullscreen) {
		struct wlr_box work, clip;
		work_area(view, &work);
		clip = (struct wlr_box){-100000, -100000, 200000, work.y + work.height - view->ly + 100000};
		if (clip.height < 1) {
			clip.height = 1;
		}
		wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, &clip);
		return;
	}
#endif
	wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node, NULL);
}

/* the window's visible top-left corner (its geometry, not its shadow) to x, y */
static void
place(struct cg_view *view, int x, int y)
{
	struct wlr_box geom = geometry(view);
	view->lx = x - geom.x;
	view->ly = y - geom.y;
	xv_of(view)->gx = geom.x;
	xv_of(view)->gy = geom.y;
	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
	}
	update_clip(view);
}

static void
centre(struct cg_view *view)
{
	struct wlr_box work, geom = geometry(view);
	work_area(view, &work);
	place(view, work.x + (work.width - geom.width) / 2, work.y + (work.height - geom.height) / 2);
	if (view->ly + geom.y < work.y) {
		place(view, view->lx + geom.x, work.y);
	}
}

static void
set_maximized(struct cg_view *view, bool on)
{
	struct cg_xdg_shell_view *xv = xv_of(view);
	struct wlr_xdg_toplevel *tl = xv->xdg_toplevel;
	struct wlr_box work, geom = geometry(view);

	work_area(view, &work);
	if (on) {
		if (!view->maximized && geom.width > 0 && geom.height > 0) {
			view->restore = (struct wlr_box){view->lx + geom.x, view->ly + geom.y, geom.width, geom.height};
		}
		view->maximized = true;
		wlr_xdg_toplevel_set_size(tl, work.width, work.height);
		wlr_xdg_toplevel_set_maximized(tl, true);
		place(view, work.x, work.y);
		return;
	}
	if (!view->maximized) {
		return;
	}
	view->maximized = false;
	if (wlr_box_empty(&view->restore)) {
		view->restore = (struct wlr_box){work.x + work.width / 6, work.y + work.height / 6, work.width * 2 / 3,
						 work.height * 2 / 3};
	}
	wlr_xdg_toplevel_set_size(tl, view->restore.width, view->restore.height);
	wlr_xdg_toplevel_set_maximized(tl, false);
	place(view, view->restore.x, view->restore.y);
	view->user_placed = true;
}

static void
set_fullscreen(struct cg_view *view, bool on)
{
	struct cg_xdg_shell_view *xv = xv_of(view);
	struct wlr_xdg_toplevel *tl = xv->xdg_toplevel;
	struct wlr_box layout, geom = geometry(view);

	if (on == xv->fullscreen) {
		wlr_xdg_toplevel_set_fullscreen(tl, on);
		return;
	}
	if (on) {
		if (geom.width > 0 && geom.height > 0) {
			xv->windowed = (struct wlr_box){view->lx + geom.x, view->ly + geom.y, geom.width, geom.height};
		}
		xv->fullscreen = true;
		view_get_layout_box(view, &layout);
		wlr_xdg_toplevel_set_size(tl, layout.width, layout.height);
		wlr_xdg_toplevel_set_fullscreen(tl, true);
		place(view, layout.x, layout.y);
		return;
	}
	xv->fullscreen = false;
	wlr_xdg_toplevel_set_fullscreen(tl, false);
	if (view->maximized) {
		view->maximized = false; /* set again, keeping its restore box */
		struct wlr_box restore = view->restore;
		set_maximized(view, true);
		view->restore = restore;
		return;
	}
	if (!wlr_box_empty(&xv->windowed)) {
		wlr_xdg_toplevel_set_size(tl, xv->windowed.width, xv->windowed.height);
		place(view, xv->windowed.x, xv->windowed.y);
	} else {
		wlr_xdg_toplevel_set_size(tl, 0, 0);
		centre(view);
	}
}

void
wayland_app_minimize(struct cg_view *view)
{
	struct cg_server *server = view->server;

	if (view->minimized) {
		return;
	}
	view->minimized = true;
	if (view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	}
	if (server->seat->grab_view == view) {
		server->seat->grab_view = NULL;
	}
	if (seat_get_focus(server->seat) == view) {
		wlr_xdg_toplevel_set_activated(xv_of(view)->xdg_toplevel, false);
		elevated_focus_session(server);
	}
}

void
wayland_app_activate(struct cg_view *view)
{
	struct cg_server *server = view->server;

	if (view->minimized) {
		view->minimized = false;
		if (view->scene_tree) {
			wlr_scene_node_set_enabled(&view->scene_tree->node, lock_view_allowed(&server->lock, view));
		}
	}
	if (view->scene_tree) {
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	}
	seat_set_focus(server->seat, view);
}

void
wayland_app_close(struct cg_view *view)
{
	wlr_xdg_toplevel_send_close(xv_of(view)->xdg_toplevel);
}

/* End task: the program's process (the one connected), when it is the
 * asking user's own */
bool
wayland_app_kill(struct cg_view *view, uid_t uid)
{
	struct wl_client *client = wl_resource_get_client(xv_of(view)->xdg_toplevel->resource);
	pid_t pid = 0;
	uid_t cuid = (uid_t) -1;
	gid_t gid;
	char proc[32];
	struct stat st;

	wl_client_get_credentials(client, &pid, &cuid, &gid);
	snprintf(proc, sizeof(proc), "/proc/%d", (int) pid);
	if (pid <= 1 || pid == getpid() || cuid != uid || stat(proc, &st) || st.st_uid != uid) {
		return false;
	}
	return kill(pid, SIGKILL) == 0;
}

unsigned long
wayland_app_window_id(struct cg_view *view)
{
	return xv_of(view)->window_id;
}

const char *
wayland_app_class(struct cg_view *view)
{
	return xv_of(view)->xdg_toplevel->app_id;
}

bool
wayland_app_listed(struct cg_view *view)
{
	/* a program's main windows, not its dialogs (they have a parent) */
	return wayland_app_is(view) && !xv_of(view)->xdg_toplevel->parent;
}

/* ---- the program's requests -------------------------------------------- */

static void
handle_request_move(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xv = wl_container_of(listener, xv, request_move);
	struct cg_view *view = &xv->view;
	struct cg_seat *seat = view->server->seat;
	struct wlr_box geom = geometry(view);
	(void) data;

	if (!view->wlr_surface || xv->fullscreen || seat->seat->pointer_state.button_count == 0) {
		return; /* only while the user holds a button */
	}
	if (view->maximized) {
		/* dragged off its maximized place: restored under the pointer */
		double fx = geom.width > 0 ? (seat->cursor->x - (view->lx + geom.x)) / geom.width : 0.5;
		set_maximized(view, false);
		place(view, (int) (seat->cursor->x - fx * view->restore.width), (int) seat->cursor->y - 10);
		geom = (struct wlr_box){0, 0, view->restore.width, view->restore.height};
	}
	seat->grab_view = view;
	seat->grab_resize = false;
	seat->grab_edges = 0;
	seat->grab_cx = seat->cursor->x;
	seat->grab_cy = seat->cursor->y;
	seat->grab_box = (struct wlr_box){view->lx + geometry(view).x, view->ly + geometry(view).y, geom.width,
					  geom.height};
}

static void
handle_request_resize(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xv = wl_container_of(listener, xv, request_resize);
	struct wlr_xdg_toplevel_resize_event *ev = data;
	struct cg_view *view = &xv->view;
	struct cg_seat *seat = view->server->seat;
	struct wlr_box geom = geometry(view);

	if (!view->wlr_surface || xv->fullscreen || view->maximized || seat->seat->pointer_state.button_count == 0) {
		return;
	}
	seat->grab_view = view;
	seat->grab_resize = true;
	seat->grab_edges = ev->edges;
	seat->grab_cx = seat->cursor->x;
	seat->grab_cy = seat->cursor->y;
	seat->grab_box = (struct wlr_box){view->lx + geom.x, view->ly + geom.y, geom.width, geom.height};
	wlr_xdg_toplevel_set_resizing(xv->xdg_toplevel, true);
}

static void
handle_request_maximize(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xv = wl_container_of(listener, xv, request_maximize);
	struct cg_view *view = &xv->view;
	(void) data;

	if (!xv->xdg_toplevel->base->initialized) {
		return;
	}
	if (!view->wlr_surface) {
		/* before it maps: maximized when it does */
		xv->want_maximized = xv->xdg_toplevel->requested.maximized;
		wlr_xdg_surface_schedule_configure(xv->xdg_toplevel->base);
		return;
	}
	set_maximized(view, xv->xdg_toplevel->requested.maximized);
	/* a configure is owed even when nothing changed */
	wlr_xdg_surface_schedule_configure(xv->xdg_toplevel->base);
}

static void
handle_request_minimize(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xv = wl_container_of(listener, xv, request_minimize);
	(void) data;
	if (xv->view.wlr_surface && xv->xdg_toplevel->requested.minimized) {
		wayland_app_minimize(&xv->view);
	}
}

void
wayland_app_request_fullscreen(struct cg_view *view)
{
	struct cg_xdg_shell_view *xv = xv_of(view);

	if (!xv->xdg_toplevel->base->initialized) {
		return;
	}
	if (!view->wlr_surface) {
		xv->want_fullscreen = xv->xdg_toplevel->requested.fullscreen;
		wlr_xdg_surface_schedule_configure(xv->xdg_toplevel->base);
		return;
	}
	set_fullscreen(view, xv->xdg_toplevel->requested.fullscreen);
}

/* ---- the user moving or resizing it (seat.c's pointer motion) ----------- */

bool
wayland_app_grab_motion(struct cg_seat *seat, double lx, double ly)
{
	struct cg_view *view = seat->grab_view;
	struct wlr_box b = seat->grab_box, work;
	int dx, dy;

	if (!wayland_app_is(view)) {
		return false;
	}
	if (seat->seat->pointer_state.button_count == 0 || !view->wlr_surface) {
		wayland_app_grab_end(seat);
		return false;
	}
	dx = (int) (lx - seat->grab_cx);
	dy = (int) (ly - seat->grab_cy);
	work_area(view, &work);
	if (!seat->grab_resize) {
		b.x += dx;
		b.y += dy;
		/* its top (where a title bar is) stays on the screen, above the taskbar */
		if (b.y < work.y) {
			b.y = work.y;
		}
		if (b.y > work.y + work.height - 32) {
			b.y = work.y + work.height - 32;
		}
		place(view, b.x, b.y);
	} else {
		if (seat->grab_edges & WLR_EDGE_LEFT) {
			b.x += dx;
			b.width -= dx;
		} else if (seat->grab_edges & WLR_EDGE_RIGHT) {
			b.width += dx;
		}
		if (seat->grab_edges & WLR_EDGE_TOP) {
			b.y += dy;
			b.height -= dy;
		} else if (seat->grab_edges & WLR_EDGE_BOTTOM) {
			b.height += dy;
		}
		if (b.width < MIN_W) {
			b.width = MIN_W;
		}
		if (b.height < MIN_H) {
			b.height = MIN_H;
		}
		/* the opposite edge stays put: placed as the program commits
		 * the new size (wayland_app_commit) */
		wlr_xdg_toplevel_set_size(xv_of(view)->xdg_toplevel, b.width, b.height);
	}
	view->user_placed = true;
	return true;
}

void
wayland_app_grab_end(struct cg_seat *seat)
{
	struct cg_view *view = seat->grab_view;
	if (wayland_app_is(view)) {
		if (seat->grab_resize) {
			wlr_xdg_toplevel_set_resizing(xv_of(view)->xdg_toplevel, false);
		}
		seat->grab_view = NULL;
	}
}

/* ---- placement ---------------------------------------------------------- */

/* the first configure: the size it asks for (0x0: its own choice), within
 * the work area, or what it asked for before mapping */
void
wayland_app_initial_configure(struct cg_view *view)
{
	struct cg_xdg_shell_view *xv = xv_of(view);
	struct wlr_xdg_toplevel *tl = xv->xdg_toplevel;
	struct wlr_box work, layout;

	work_area(view, &work);
	/* what the program's xdg_wm_base version knows of (wlroots asserts it) */
	if (wl_resource_get_version(tl->resource) >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
		wlr_xdg_toplevel_set_wm_capabilities(tl, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
								 WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE |
								 WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
	}
	if (wl_resource_get_version(tl->resource) >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION) {
		wlr_xdg_toplevel_set_bounds(tl, work.width, work.height);
	}
	if (tl->requested.fullscreen || xv->want_fullscreen) {
		view_get_layout_box(view, &layout);
		wlr_xdg_toplevel_set_size(tl, layout.width, layout.height);
		wlr_xdg_toplevel_set_fullscreen(tl, true);
		xv->want_fullscreen = true;
	} else if (tl->requested.maximized || xv->want_maximized) {
		wlr_xdg_toplevel_set_size(tl, work.width, work.height);
		wlr_xdg_toplevel_set_maximized(tl, true);
		xv->want_maximized = true;
	} else {
		wlr_xdg_toplevel_set_size(tl, 0, 0);
	}
}

/* mapped, or the screen changed */
void
wayland_app_place(struct cg_view *view)
{
	struct cg_xdg_shell_view *xv = xv_of(view);
	struct wlr_box work, layout, geom = geometry(view);

	work_area(view, &work);
	if (xv->want_fullscreen) {
		xv->want_fullscreen = false;
		xv->fullscreen = true;
	}
	if (xv->fullscreen) {
		view_get_layout_box(view, &layout);
		wlr_xdg_toplevel_set_size(xv->xdg_toplevel, layout.width, layout.height);
		place(view, layout.x, layout.y);
		return;
	}
	if (xv->want_maximized || view->maximized ||
	    (!view->user_placed && (geom.width > work.width || geom.height > work.height))) {
		/* larger than the work area: maximized there, as X programs
		 * that size themselves to the screen are (view_position) */
		struct wlr_box restore = view->restore;
		xv->want_maximized = false;
		view->maximized = false;
		set_maximized(view, true);
		if (!wlr_box_empty(&restore)) {
			view->restore = restore;
		}
		if (wlr_box_empty(&view->restore) || view->restore.width >= work.width ||
		    view->restore.height >= work.height) {
			view->restore = (struct wlr_box){work.x + work.width / 6, work.y + work.height / 6,
							 work.width * 2 / 3, work.height * 2 / 3};
		}
		return;
	}
	if (!view->user_placed) {
		centre(view);
		return;
	}
	update_clip(view);
}

/* every commit: a new size placed -- centred while the user has not moved
 * it, the edge opposite the one dragged kept -- and the clip kept */
void
wayland_app_commit(struct cg_view *view)
{
	struct cg_xdg_shell_view *xv = xv_of(view);
	struct cg_seat *seat = view->server->seat;
	struct wlr_box geom;

	if (!view->wlr_surface || !view->scene_tree) {
		return;
	}
	geom = geometry(view);
	if (geom.width == xv->last_w && geom.height == xv->last_h) {
		return;
	}
	xv->last_w = geom.width;
	xv->last_h = geom.height;
	if (seat->grab_view == view && seat->grab_resize) {
		struct wlr_box b = seat->grab_box;
		int x = b.x, y = b.y;
		if (seat->grab_edges & WLR_EDGE_LEFT) {
			x = b.x + b.width - geom.width;
		}
		if (seat->grab_edges & WLR_EDGE_TOP) {
			y = b.y + b.height - geom.height;
		}
		place(view, x, y);
	} else if (!view->maximized && !xv->fullscreen && !view->user_placed) {
		centre(view);
	} else {
		/* its shadow may have changed: the visible corner stays */
		place(view, view->lx + xv->gx, view->ly + xv->gy);
	}
}

/* ---- life --------------------------------------------------------------- */

void
wayland_app_init(struct cg_xdg_shell_view *xv)
{
	static unsigned long serial;
	struct cg_server *server = xv->view.server;
	struct wl_client *client = wl_resource_get_client(xv->xdg_toplevel->resource);

	/* the lock screen, a consent prompt, Remote Desktop: privileged
	 * clients keep cage's full-screen windows */
	if (lock_client_is_privileged(&server->lock, client)) {
		return;
	}
	xv->managed = true;
	xv->window_id = 0x80000000UL | (++serial & 0x0fffffffUL);

	xv->request_move.notify = handle_request_move;
	wl_signal_add(&xv->xdg_toplevel->events.request_move, &xv->request_move);
	xv->request_resize.notify = handle_request_resize;
	wl_signal_add(&xv->xdg_toplevel->events.request_resize, &xv->request_resize);
	xv->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&xv->xdg_toplevel->events.request_maximize, &xv->request_maximize);
	xv->request_minimize.notify = handle_request_minimize;
	wl_signal_add(&xv->xdg_toplevel->events.request_minimize, &xv->request_minimize);
}

void
wayland_app_fini(struct cg_xdg_shell_view *xv)
{
	if (!xv->managed) {
		return;
	}
	wl_list_remove(&xv->request_move.link);
	wl_list_remove(&xv->request_resize.link);
	wl_list_remove(&xv->request_maximize.link);
	wl_list_remove(&xv->request_minimize.link);
}
