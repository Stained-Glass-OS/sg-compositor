/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "decor.h"
#include "elevated.h"
#include "server.h"
#include "session_x11.h"
#include "view.h"
#include "xwayland.h"

struct cg_xwayland_view *
xwayland_view_from_view(struct cg_view *view)
{
	return (struct cg_xwayland_view *) view;
}

bool
xwayland_view_should_manage(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	struct wlr_xwayland_surface *xwayland_surface = xwayland_view->xwayland_surface;
	return !xwayland_surface->override_redirect;
}

/* Wine's virtual desktop window, the session's shell: explorer's top-level
 * window "<name> - Wine Desktop" (its Motif hints ask for a title, which it
 * never gets: see decor.c) */
bool
xwayland_view_is_shell_desktop(struct cg_view *view)
{
	struct wlr_xwayland_surface *xs = xwayland_view_from_view(view)->xwayland_surface;
	return xs && !view->elevated && !xs->override_redirect && !xs->parent && xs->class &&
	       !strcmp(xs->class, "explorer.exe") && xs->title && strstr(xs->title, " Desktop");
}

static char *
get_title(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	return xwayland_view->xwayland_surface->title;
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;
	if (xsurface->surface == NULL) {
		*width_out = 0;
		*height_out = 0;
		return;
	}

	*width_out = xsurface->surface->current.width;
	*height_out = xsurface->surface->current.height;
}

static bool
is_primary(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	struct wlr_xwayland_surface *parent = xwayland_view->xwayland_surface->parent;
	return parent == NULL;
}

static bool
is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	if (parent->type != CAGE_XDG_SHELL_VIEW) {
		return false;
	}
	struct cg_xwayland_view *_child = xwayland_view_from_view(child);
	struct wlr_xwayland_surface *xwayland_surface = _child->xwayland_surface;
	struct cg_xwayland_view *_parent = xwayland_view_from_view(parent);
	struct wlr_xwayland_surface *parent_xwayland_surface = _parent->xwayland_surface;
	while (xwayland_surface) {
		if (xwayland_surface->parent == parent_xwayland_surface) {
			return true;
		}
		xwayland_surface = xwayland_surface->parent;
	}
	return false;
}

static void
activate(struct cg_view *view, bool activate)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	wlr_xwayland_surface_activate(xwayland_view->xwayland_surface, activate);
	/* sg-compositor: the window activated is on top for X too (clicks) */
	if (activate && !view->elevated && !xwayland_view->xwayland_surface->override_redirect)
		wlr_xwayland_surface_restack(xwayland_view->xwayland_surface, NULL, XCB_STACK_MODE_ABOVE);
	decor_set_active(view, activate);
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	wlr_xwayland_surface_configure(xwayland_view->xwayland_surface, view->lx, view->ly, output_width,
				       output_height);
	wlr_xwayland_surface_set_maximized(xwayland_view->xwayland_surface, true);
}

static void
destroy(struct cg_view *view)
{
	struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
	free(xwayland_view);
}

static void
handle_xwayland_surface_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, request_fullscreen);
	struct wlr_xwayland_surface *xwayland_surface = xwayland_view->xwayland_surface;
	wlr_xwayland_surface_set_fullscreen(xwayland_view->xwayland_surface, xwayland_surface->fullscreen);
	decor_set_fullscreen(&xwayland_view->view, xwayland_surface->fullscreen);
}

/* sg-compositor: an override-redirect window moves itself; follow it. */
static void
handle_or_set_geometry(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, set_geometry);
	struct cg_view *view = &xwayland_view->view;

	view->lx = xwayland_view->xwayland_surface->x;
	view->ly = xwayland_view->xwayland_surface->y;
	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, view->lx, view->ly);
	}
}

/* sg-compositor: a managed X11 window of a Linux program (a terminal) gets
 * the size it asks for, and stays centred -- xterm maps at 1x1 and then asks
 * for its real size. Wine's windows are override-redirect and move
 * themselves; an elevated display's windows have their own handler. */
static void
handle_xwayland_surface_request_configure(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	struct cg_view *view = &xwayland_view->view;

	wlr_xwayland_surface_configure(event->surface, event->x, event->y, event->width < 1 ? 1 : event->width,
				       event->height < 1 ? 1 : event->height);
	if (view->scene_tree) {
		view_position(view);
		wlr_xwayland_surface_configure(event->surface, view->lx, view->ly, event->surface->width,
					       event->surface->height);
	}
}

/* sg-compositor: a Linux program's window that Wine put into a window of its
 * own (wine-sg's embedding, winex11 sg_embed.c) is no longer ours to manage.
 * wlroots' window manager still listens to it, and a focus change into it
 * (Wine giving the program the keyboard) was undone at once -- "X clients
 * must not change the focus behind the compositor's back" -- so the program
 * got no keys. Not listened to any more; listened to again should it come
 * back to the root as a top-level (wlroots' own mask, xwm.c). */
static void
xwm_listen(struct cg_view *view, bool on)
{
#if CAGE_HAS_XWAYLAND
	struct wlr_xwayland_surface *xs = xwayland_view_from_view(view)->xwayland_surface;
	xcb_connection_t *c = view->server->xwayland ? wlr_xwayland_get_xwm_connection(view->server->xwayland) : NULL;
	xcb_query_tree_reply_t *tree;
	uint32_t mask = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE;

	if (!c || !xs || xs->override_redirect || view->elevated) {
		return;
	}
	if (!on) {
		if (!(tree = xcb_query_tree_reply(c, xcb_query_tree(c, xs->window_id), NULL))) {
			return;
		}
		on = tree->parent == tree->root;   /* an ordinary unmap: still ours */
		free(tree);
		if (on) {
			return;
		}
		mask = 0;
	}
#ifndef SG_MUTANT_EMBED_FOCUS_FIGHT
	xcb_change_window_attributes(c, xs->window_id, XCB_CW_EVENT_MASK, &mask);
	xcb_flush(c);
#endif
#endif
}

static void
handle_xwayland_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, unmap);
	struct cg_view *view = &xwayland_view->view;

	xwm_listen(view, false);

	if (xwayland_view->or_geometry) {
		wl_list_remove(&xwayland_view->set_geometry.link);
		xwayland_view->or_geometry = false;
	}
	decor_destroy(view);
	session_x11_unhold(view);
	view_unmap(view);
}

static void
handle_xwayland_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, map);
	struct cg_view *view = &xwayland_view->view;

	if (!xwayland_view_should_manage(view)) {
		view->lx = xwayland_view->xwayland_surface->x;
		view->ly = xwayland_view->xwayland_surface->y;
		if (!view->elevated && !xwayland_view->or_geometry) {
			xwayland_view->set_geometry.notify = handle_or_set_geometry;
			wl_signal_add(&xwayland_view->xwayland_surface->events.set_geometry, &xwayland_view->set_geometry);
			xwayland_view->or_geometry = true;
		}
	}

	xwm_listen(view, true);
	view_map(view, xwayland_view->xwayland_surface->surface);

	/* sg-compositor: a managed window is where the compositor put it, in X
	 * too, and above the others there: X picks the window a click goes to by
	 * its own idea of positions and stacking. Centred only on screen, xev
	 * and sdl-freerdp's dialogs were still at 0,0 under the full-screen Wine
	 * desktop for X, and every click went to Wine. */
	if (xwayland_view_should_manage(view) && !view->elevated) {
		struct wlr_xwayland_surface *xs = xwayland_view->xwayland_surface;
		/* a Linux program's window gets a title bar (decor.c), and is
		 * centred with it */
		decor_create(view);
		if (view->decorated) {
			view_position(view);
		}
		wlr_xwayland_surface_configure(xs, view->lx, view->ly, xs->width, xs->height);
		wlr_xwayland_surface_restack(xs, NULL, XCB_STACK_MODE_ABOVE);
		/* the taskbar frames it in a moment: shown then, in its frame */
		session_x11_hold(view);
	}
}

static void
handle_xwayland_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, destroy);
	struct cg_view *view = &xwayland_view->view;

	if (xwayland_view->or_geometry) {
		wl_list_remove(&xwayland_view->set_geometry.link);
		xwayland_view->or_geometry = false;
	}
	decor_destroy(view);
	session_x11_unhold(view);
	wl_list_remove(&xwayland_view->destroy.link);
	wl_list_remove(&xwayland_view->request_fullscreen.link);
	if (view->elevated) {
		elevated_view_unlisten(xwayland_view);
	} else {
		wl_list_remove(&xwayland_view->request_configure.link);
		session_view_unlisten(xwayland_view);
	}
	xwayland_view->xwayland_surface = NULL;

	view_destroy(view);
}

static const struct cg_view_impl xwayland_view_impl = {
	.get_title = get_title,
	.get_geometry = get_geometry,
	.is_primary = is_primary,
	.is_transient_for = is_transient_for,
	.activate = activate,
	.maximize = maximize,
	.destroy = destroy,
};

void
handle_xwayland_associate(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, associate);
	struct wlr_xwayland_surface *xsurface = xwayland_view->xwayland_surface;

	xwayland_view->map.notify = handle_xwayland_surface_map;
	wl_signal_add(&xsurface->surface->events.map, &xwayland_view->map);
	xwayland_view->unmap.notify = handle_xwayland_surface_unmap;
	wl_signal_add(&xsurface->surface->events.unmap, &xwayland_view->unmap);
}

void
handle_xwayland_dissociate(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xwayland_view = wl_container_of(listener, xwayland_view, dissociate);
	wl_list_remove(&xwayland_view->map.link);
	wl_list_remove(&xwayland_view->unmap.link);
}

void
xwayland_view_create(struct cg_server *server, struct wlr_xwayland_surface *xwayland_surface,
		     struct cg_elevated *elevated)
{
	struct cg_xwayland_view *xwayland_view = calloc(1, sizeof(struct cg_xwayland_view));
	if (!xwayland_view) {
		wlr_log(WLR_ERROR, "Failed to allocate XWayland view");
		return;
	}

	view_init(&xwayland_view->view, server, CAGE_XWAYLAND_VIEW, &xwayland_view_impl);
	xwayland_view->xwayland_surface = xwayland_surface;
	/* sg-compositor: a window of an elevated program's own display. */
	xwayland_view->view.elevated = elevated;

	xwayland_view->associate.notify = handle_xwayland_associate;
	wl_signal_add(&xwayland_surface->events.associate, &xwayland_view->associate);
	xwayland_view->dissociate.notify = handle_xwayland_dissociate;
	wl_signal_add(&xwayland_surface->events.dissociate, &xwayland_view->dissociate);
	xwayland_view->destroy.notify = handle_xwayland_surface_destroy;
	wl_signal_add(&xwayland_surface->events.destroy, &xwayland_view->destroy);
	xwayland_view->request_fullscreen.notify = handle_xwayland_surface_request_fullscreen;
	wl_signal_add(&xwayland_surface->events.request_fullscreen, &xwayland_view->request_fullscreen);
	if (elevated) {
		elevated_view_listen(xwayland_view);
	} else {
		xwayland_view->request_configure.notify = handle_xwayland_surface_request_configure;
		wl_signal_add(&xwayland_surface->events.request_configure, &xwayland_view->request_configure);
		session_view_listen(xwayland_view);
	}
}

void
handle_xwayland_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xwayland_surface);
	xwayland_view_create(server, data, NULL);
}
