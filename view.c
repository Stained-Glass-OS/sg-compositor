/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2021 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

#include "decor.h"
#include "elevated.h"
#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "backdrop.h"
#include "wayland_app.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

char *
view_get_title(struct cg_view *view)
{
	const char *title = view->impl->get_title(view);
	if (!title) {
		return NULL;
	}
	return strndup(title, strlen(title));
}

bool
view_is_primary(struct cg_view *view)
{
	return view->impl->is_primary(view);
}

bool
view_is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	return child->impl->is_transient_for(child, parent);
}

void
view_activate(struct cg_view *view, bool activate)
{
	view->impl->activate(view, activate);
}

static bool
view_extends_output_layout(struct cg_view *view, struct wlr_box *layout_box)
{
	int width, height;
	view->impl->get_geometry(view, &width, &height);

	return (layout_box->height < height || layout_box->width < width);
}

static void
view_maximize(struct cg_view *view, struct wlr_box *layout_box)
{
	view->lx = layout_box->x;
	view->ly = layout_box->y;

	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
	}

	view->impl->maximize(view, layout_box->width, layout_box->height);
}

static void
view_center(struct cg_view *view, struct wlr_box *layout_box)
{
	int width, height;
	view->impl->get_geometry(view, &width, &height);

	view->lx = layout_box->x + (layout_box->width - width) / 2;
	view->ly = layout_box->y + (layout_box->height - height) / 2;
	/* the title bar above it is part of what is centred */
	if (view->decorated) {
		view->ly = layout_box->y + (layout_box->height - height - decor_title_h()) / 2 + decor_title_h();
		if (view->ly < layout_box->y + decor_title_h()) {
			view->ly = layout_box->y + decor_title_h();
		}
	}

	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
	}
}

/* Where a view lives: the whole layout, or -- while Remote Desktop has the
 * session -- the remote output for the user's windows and the console for
 * privileged ones (the lock screen). */
static void
view_layout_box(struct cg_view *view, struct wlr_box *box)
{
	struct cg_server *server = view->server;

	if (server->remote && server->remote_output) {
		if (!lock_view_is_privileged(&server->lock, view)) {
			wlr_output_layout_get_box(server->output_layout, server->remote_output, box);
			if (!wlr_box_empty(box)) {
				return;
			}
		} else {
			struct cg_output *output;
			wl_list_for_each (output, &server->outputs, link) {
				if (output->wlr_output == server->remote_output) {
					continue;
				}
				wlr_output_layout_get_box(server->output_layout, output->wlr_output, box);
				if (!wlr_box_empty(box)) {
					return;
				}
			}
		}
	}
	wlr_output_layout_get_box(server->output_layout, NULL, box);
}

void
view_get_layout_box(struct cg_view *view, struct wlr_box *box)
{
	view_layout_box(view, box);
}

/* The display a window is on -- the one under its middle, else under the
 * pointer, else the one at the layout's origin -- for what fills "the
 * screen": maximized, full screen, the work area above the taskbar. With
 * several displays these filled all of them (David 2026-11-13: Firefox and
 * the Remote Desktop client full screen across every display). While
 * Remote Desktop has the session, its own box. */
void
view_get_output_box(struct cg_view *view, struct wlr_box *box)
{
	struct wlr_box layout;

	view_layout_box(view, &layout);
#ifndef SG_MUTANT_FILL_ALL_DISPLAYS
	struct cg_server *server = view->server;
	if (!(server->remote && server->remote_output)) {
		struct wlr_output *at = NULL;
		int w = 0, h = 0;
		if (view->impl->get_geometry) {
			view->impl->get_geometry(view, &w, &h);
		}
		if (w > 0 && h > 0) {
			at = wlr_output_layout_output_at(server->output_layout, view->lx + w / 2, view->ly + h / 2);
		}
		if (!at) {
			at = wlr_output_layout_output_at(server->output_layout, server->seat->cursor->x, server->seat->cursor->y);
		}
		if (!at) {
			at = wlr_output_layout_output_at(server->output_layout, layout.x, layout.y);
		}
		if (at) {
			wlr_output_layout_get_box(server->output_layout, at, box);
			if (!wlr_box_empty(box)) {
				return;
			}
		}
	}
#endif
	*box = layout;
}

void
view_position(struct cg_view *view)
{
	struct wlr_box layout_box;

	/* sg-compositor: a program's native Wayland window: in the work area,
	 * as a window (wayland_app.c), not over the whole screen */
	if (wayland_app_is(view)) {
		wayland_app_place(view);
		return;
	}
	view_layout_box(view, &layout_box);

	/* sg-compositor: an X11 window of a Linux program (a terminal) keeps the
	 * size it asks for, centred, when it fits: maximized, xterm opened as a
	 * wall of text across the whole screen. Wine's own windows place
	 * themselves (override-redirect) and never come here. */
#if CAGE_HAS_XWAYLAND
	/* the session's shell -- Wine's desktop window, explorer's -- is the
	 * screen: at its origin, whatever size Wine gives it. Centred like a
	 * Linux program's window, it stayed at the old offset when the
	 * resolution changed (David: Display settings in a VM). */
	if (view->type == CAGE_XWAYLAND_VIEW && xwayland_view_is_shell_desktop(view)) {
		view->lx = layout_box.x;
		view->ly = layout_box.y;
		if (view->scene_tree) {
			wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
		}
		/* full screen (as Wine keeps it): every display, at once when they
		 * are rearranged, turned or change mode -- left at the old size,
		 * the desktop ran past a display moved left of the main one, or
		 * stopped short of it (GitHub #12) */
#ifndef SG_MUTANT_SHELL_DESKTOP_ONE_DISPLAY
		xwayland_view_fill_layout(view, &layout_box);
#endif
		return;
	}
	/* where the user put it with its title bar stays (the compositor does
	 * not re-centre a window the user moved or maximized) */
	if (view->type == CAGE_XWAYLAND_VIEW && view->user_placed) {
		return;
	}
	/* with several displays (David 2026-11-13: the sign-in screen sat
	 * across two, half on each): a window that asks for exactly one
	 * display's area -- the sign-in screen, a full-screen program -- is put
	 * on that display; another is centred on one display, the one the
	 * pointer is on (else the one at the layout's origin, the main one
	 * when none is set), not across the gap between them */
#ifndef SG_MUTANT_SPAN_CENTRED
	if (view->type == CAGE_XWAYLAND_VIEW && !view->server->remote) {
		struct wlr_box want, box, one = {0};
		struct cg_output *output;
		struct wlr_output *at;
		bool asked = xwayland_view_x_box(view, &want);
		wl_list_for_each (output, &view->server->outputs, link) {
			wlr_output_layout_get_box(view->server->output_layout, output->wlr_output, &box);
			if (asked && !wlr_box_empty(&box) && box.x == want.x && box.y == want.y &&
			    box.width == want.width && box.height == want.height) {
				/* and the sign-in words go from the other displays:
				 * "Welcome" stayed beside the sign-in screen */
				backdrop_shell_up(view->server);
				view->lx = box.x;
				view->ly = box.y;
				if (view->scene_tree) {
					wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
				}
				return;
			}
		}
		at = wlr_output_layout_output_at(view->server->output_layout, view->server->seat->cursor->x,
						 view->server->seat->cursor->y);
		if (!at) {
			at = wlr_output_layout_output_at(view->server->output_layout, 0, 0);
		}
		if (at) {
			wlr_output_layout_get_box(view->server->output_layout, at, &one);
		}
		if (!wlr_box_empty(&one) && !view_extends_output_layout(view, &one)) {
			view_center(view, &one);
			return;
		}
		/* bigger than one display: maximized on it (below), not centred
		 * across several */
		if (!wlr_box_empty(&one)) {
			goto too_big;
		}
	}
#endif
	if (view->type == CAGE_XWAYLAND_VIEW && !view_extends_output_layout(view, &layout_box)) {
		view_center(view, &layout_box);
		return;
	}
	/* one larger than the screen (a program that sizes itself to the
	 * screen: SG Office's editors) is maximized as the title bar's
	 * Maximize does -- above the taskbar, below its own title bar -- not
	 * over the taskbar; restored, it takes two thirds, centred */
#ifndef SG_MUTANT_SPAN_CENTRED
too_big:
#endif
	if (view->type == CAGE_XWAYLAND_VIEW) {
		struct wlr_box box;
		view_get_output_box(view, &box);
		box.height -= DECOR_TASKBAR_H;
		if (view->decorated) {
			box.y += decor_title_h();
			box.height -= decor_title_h();
		}
		if (!view->maximized) {
			view->restore = (struct wlr_box){box.x + box.width / 6, box.y + box.height / 6, box.width * 2 / 3,
							 box.height * 2 / 3};
		}
		view->maximized = true;
		view_maximize(view, &box);
		return;
	}
#endif
	if (view_is_primary(view) || view_extends_output_layout(view, &layout_box)) {
		view_maximize(view, &layout_box);
	} else {
		view_center(view, &layout_box);
	}
}

void
view_position_all(struct cg_server *server)
{
	struct cg_view *view;
	wl_list_for_each (view, &server->views, link) {
		if (view->elevated) {
			elevated_view_place(view);
		} else {
			view_position(view);
		}
	}
}

void
view_unmap(struct cg_view *view)
{
	struct cg_server *server = view->server;
	bool had_focus = view->wlr_surface && server->seat->seat->keyboard_state.focused_surface == view->wlr_surface;

	wl_list_remove(&view->link);

	if (view->server->seat->grab_view == view) {
		view->server->seat->grab_view = NULL;
	}
	wlr_scene_node_destroy(&view->scene_tree->node);
#ifndef SG_MUTANT_UNMAP_DANGLING_TREE
	/* sg-compositor: gone with it -- every "if (view->scene_tree)" after
	   this (a hidden X window asking to be moved, a lock, the taskbar's
	   hold) used freed memory: a Wine message box closing at 2736x1824
	   took the login screen's compositor down in scene_node_get_root
	   (regression walk 2026-10-06) */
	view->scene_tree = NULL;
#endif

	view->wlr_surface->data = NULL;
	view->wlr_surface = NULL;

	/* sg-compositor: the keyboard goes on to the window now in front. A
	   window that unmaps but lives on -- a Linux program's window taken
	   into Wine's frame for it (wine-sg's embedding), which had the keyboard
	   from its own map -- left it with a surface gone: no keys reached the
	   program until a click (David 2026-10-03). view_destroy does this only
	   when the window is destroyed. */
	(void)had_focus;
#ifndef SG_MUTANT_UNMAP_KEEPS_FOCUS
	if (had_focus) {
		struct cg_view *next;
		wl_list_for_each (next, &server->views, link) {
			bool unmanaged = false;
#if CAGE_HAS_XWAYLAND
			unmanaged = next->type == CAGE_XWAYLAND_VIEW && !xwayland_view_should_manage(next);
#endif
			if (!unmanaged && next->wlr_surface) {
				seat_set_focus(server->seat, next);
				break;
			}
		}
	}
#endif
}

void
view_map(struct cg_view *view, struct wlr_surface *surface)
{
	struct cg_server *server = view->server;
	struct wlr_scene_tree *parent;

	view->wlr_surface = surface;
	surface->data = view;

	/* sg-compositor: the session's windows at the bottom, elevated programs'
	 * above them, privileged ones (lock screen, consent prompt) on top. */
	if (view->elevated) {
		parent = server->elevated_tree;
	} else if (lock_view_is_privileged(&server->lock, view)) {
		parent = server->privileged_tree;
	} else {
		parent = server->normal_tree;
	}
	view->scene_tree = wlr_scene_subsurface_tree_create(parent, surface);
	if (!view->scene_tree) {
		view->wlr_surface = NULL;
		surface->data = NULL;
		wl_resource_post_no_memory(surface->resource);
		return;
	}
	view->scene_tree->node.data = view;

#if CAGE_HAS_XWAYLAND
	/* the desktop is up: the sign-in words go from the backdrop */
	if (view->type == CAGE_XWAYLAND_VIEW && xwayland_view_is_shell_desktop(view)) {
		backdrop_shell_up(server);
	}
#endif

	bool unmanaged = false;
#if CAGE_HAS_XWAYLAND
	unmanaged = view->type == CAGE_XWAYLAND_VIEW && !xwayland_view_should_manage(view);
#endif
	if (view->elevated) {
		elevated_view_mapped(view);
	} else if (unmanaged) {
		/* sg-compositor: override-redirect windows (a combo box's list,
		   menus, tooltips) go where X put them. */
		wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
	} else {
		view_position(view);
	}

	wl_list_insert(&server->views, &view->link);
	/* sg-compositor: a window opened during a lock stays hidden. */
	lock_view_mapped(&server->lock, view);
	/* sg-compositor: and they never take the focus. The program keeps it --
	   Wine closes a combo box's list as soon as its window loses it, and
	   the list that took it left nothing focused once it closed. */
	if (!unmanaged) {
		seat_set_focus(server->seat, view);
	}
}

void
view_destroy(struct cg_view *view)
{
	struct cg_server *server = view->server;

	if (view->wlr_surface != NULL) {
		view_unmap(view);
	}

	view->impl->destroy(view);

	/* If there is a previous view in the list, focus that. */
	bool empty = wl_list_empty(&server->views);
	if (!empty) {
		struct cg_view *prev = wl_container_of(server->views.next, prev, link);
		seat_set_focus(server->seat, prev);
	}
}

void
view_init(struct cg_view *view, struct cg_server *server, enum cg_view_type type, const struct cg_view_impl *impl)
{
	view->server = server;
	view->type = type;
	view->impl = impl;
}

struct cg_view *
view_from_wlr_surface(struct wlr_surface *surface)
{
	assert(surface);
	return surface->data;
}
