/*
 * sg-compositor: the session's own X11 programs' windows -- a Linux terminal,
 * any X program started in the session -- for Wine's taskbar. Explorer's
 * taskbar sees only Wine's windows, which live inside Wine's desktop window;
 * a Linux program's window is a top-level of its own beside it, and had no
 * button (QA: the Linux terminal "does not show up" on the taskbar).
 *
 * The control socket's XWINDOWS lists them (window id, shown or minimized,
 * focused or not, class, title), and XACTIVATE, XMINIMIZE and XCLOSE act on
 * one by its X window id; sg-lockctl carries them for the taskbar. Anyone in
 * the session may ask, as for ACTIVATE: it is focus, not input.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config.h"

#include "session_x11.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland.h>

#include "elevated.h"
#include "lock.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "xwayland.h"

#if CAGE_HAS_XWAYLAND
static struct wlr_xwayland_surface *
program_surface(struct cg_view *view)
{
	struct wlr_xwayland_surface *xs;
	size_t n;

	if (view->type != CAGE_XWAYLAND_VIEW || view->elevated) {
		return NULL;
	}
	xs = xwayland_view_from_view(view)->xwayland_surface;
	if (!xs || xs->override_redirect || xs->parent || xwayland_view_is_shell_desktop(view)) {
		return NULL;
	}
	if (lock_view_is_privileged(&view->server->lock, view)) {
		return NULL;
	}
	/* a Wine program's own top-level (its class is its .exe) has a button already */
	if (xs->class && (n = strlen(xs->class)) > 4 && !strcasecmp(xs->class + n - 4, ".exe")) {
		return NULL;
	}
	return xs;
}

static struct cg_view *
find(struct cg_server *server, unsigned long window)
{
	struct cg_view *view;
	struct wlr_xwayland_surface *xs;

	wl_list_for_each (view, &server->views, link) {
		if ((xs = program_surface(view)) && xs->window_id == window) {
			return view;
		}
	}
	return NULL;
}

static void
copy_clean(char *out, size_t len, const char *in)
{
	size_t i = 0;

	for (; in && *in && i < len - 1; in++) {
		out[i++] = (unsigned char) *in < 0x20 ? '?' : *in;
	}
	out[i] = 0;
}

size_t
session_x11_list(struct cg_server *server, char *buf, size_t len)
{
	struct cg_view *view, *focus = seat_get_focus(server->seat);
	struct wlr_xwayland_surface *xs;
	size_t off = 0;

	wl_list_for_each (view, &server->views, link) {
		char title[120], class[48];
		int n;

		if (!(xs = program_surface(view))) {
			continue;
		}
		copy_clean(title, sizeof(title), xs->title);
		copy_clean(class, sizeof(class), xs->class);
		n = snprintf(buf + off, len - off, "%u %s %s %s\t%s\n", (unsigned) xs->window_id,
			     view->minimized ? "minimized" : "shown", view == focus ? "focused" : "-",
			     class[0] ? class : "-", title);
		if (n < 0 || (size_t) n >= len - off - 5) {
			break;
		}
		off += (size_t) n;
	}
	off += (size_t) snprintf(buf + off, len - off, "END\n");
	return off;
}

bool
session_x11_activate(struct cg_server *server, unsigned long window)
{
	struct cg_view *view = find(server, window);

	if (!view) {
		return false;
	}
	if (view->minimized) {
		view->minimized = false;
		wlr_xwayland_surface_set_minimized(xwayland_view_from_view(view)->xwayland_surface, false);
		if (view->scene_tree) {
			wlr_scene_node_set_enabled(&view->scene_tree->node, lock_view_allowed(&server->lock, view));
		}
	}
	if (view->scene_tree) {
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	}
	/* X's stacking as the screen's: the shell's desktop may have been
	 * brought above it (XDESKTOP) */
	wlr_xwayland_surface_restack(xwayland_view_from_view(view)->xwayland_surface, NULL, XCB_STACK_MODE_ABOVE);
	seat_set_focus(server->seat, view);
	return true;
}

/* The shell's desktop -- Wine's desktop window, with every Wine program's
 * window in it -- in front of the session's Linux programs' windows: the
 * taskbar asks when a Wine window is brought forward (its button, Start, a
 * program starting) while a Linux program's window is in front of it. They
 * stay shown and on the taskbar; XACTIVATE brings one forward again.
 * Elevated windows and the lock screen are in layers above and stay there. */
bool
session_x11_desktop_front(struct cg_server *server)
{
	struct cg_view *view;

	wl_list_for_each (view, &server->views, link) {
		if (view->type != CAGE_XWAYLAND_VIEW || !xwayland_view_is_shell_desktop(view)) {
			continue;
		}
#ifndef SG_MUTANT_DESKTOP_NOT_RAISED
		if (view->scene_tree) {
			wlr_scene_node_raise_to_top(&view->scene_tree->node);
		}
		wlr_xwayland_surface_restack(xwayland_view_from_view(view)->xwayland_surface, NULL, XCB_STACK_MODE_ABOVE);
#endif
		seat_set_focus(server->seat, view);
		return true;
	}
	return false;
}

bool
session_x11_minimize(struct cg_server *server, unsigned long window)
{
	struct cg_view *view = find(server, window);

	if (!view) {
		return false;
	}
	if (!view->minimized) {
		view->minimized = true;
		wlr_xwayland_surface_set_minimized(xwayland_view_from_view(view)->xwayland_surface, true);
		if (view->scene_tree) {
			wlr_scene_node_set_enabled(&view->scene_tree->node, false);
		}
		if (server->seat->grab_view == view) {
			server->seat->grab_view = NULL;
		}
		if (seat_get_focus(server->seat) == view) {
			elevated_focus_session(server);
		}
	}
	return true;
}

bool
session_x11_close(struct cg_server *server, unsigned long window)
{
	struct cg_view *view = find(server, window);

	if (!view) {
		return false;
	}
	wlr_xwayland_surface_close(xwayland_view_from_view(view)->xwayland_surface);
	return true;
}
#else
size_t
session_x11_list(struct cg_server *server, char *buf, size_t len)
{
	(void) server;
	return (size_t) snprintf(buf, len, "END\n");
}
bool session_x11_activate(struct cg_server *server, unsigned long window) { (void) server; (void) window; return false; }
bool session_x11_minimize(struct cg_server *server, unsigned long window) { (void) server; (void) window; return false; }
bool session_x11_close(struct cg_server *server, unsigned long window) { (void) server; (void) window; return false; }
bool session_x11_desktop_front(struct cg_server *server) { (void) server; return false; }
#endif
