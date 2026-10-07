/*
 * sg-compositor: the session's own X11 programs' windows -- a Linux terminal,
 * any X program started in the session -- for Wine's taskbar. Explorer's
 * taskbar sees only Wine's windows, which live inside Wine's desktop window;
 * a Linux program's window is a top-level of its own beside it, and had no
 * button (QA: the Linux terminal "does not show up" on the taskbar).
 *
 * The control socket's XWINDOWS lists them (window id, shown or minimized,
 * focused or not, class, title), and XACTIVATE, XMINIMIZE, XCLOSE and XKILL act on
 * one by its X window id; sg-lockctl carries them for the taskbar. Anyone in
 * the session may ask, as for ACTIVATE: it is focus, not input.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config.h"

#include "session_x11.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland.h>
#include <xcb/xcb.h>

#include "elevated.h"
#include "lock.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#include "wayland_app.h"
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
		/* a program's native Wayland window (wayland_app.c) */
		if (wayland_app_is(view) && wayland_app_window_id(view) == window) {
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

static uint64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

/* when the taskbar last asked for the list: it frames the windows it lists
 * (wine-sg 0762) while it asks */
static uint64_t last_list_ms;

/* A Linux program's window the taskbar is about to put into a Wine frame is
 * not shown until then: it was shown with our title bar for a second or two
 * first, then in the frame (David 2026-10-02: the terminal "starts with one
 * window decorator, then changes a bit later to look like the others").
 * Framed, it is unmapped here; not framed by then (a window the taskbar
 * cannot frame, a taskbar not running), it is shown. */
#define HOLD_MS 2500
struct hold {
	struct wl_list link;
	struct cg_view *view;
	struct wl_event_source *timer;
};
static struct wl_list holds = {&holds, &holds};

static void
end_hold(struct hold *h, bool show)
{
	struct cg_view *view = h->view;
	wl_list_remove(&h->link);
	wl_event_source_remove(h->timer);
	free(h);
	if (show && view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node,
					   !view->minimized && lock_view_allowed(&view->server->lock, view));
	}
}

static int
hold_expired(void *data)
{
	end_hold(data, true);
	return 0;
}

void
session_x11_hold(struct cg_view *view)
{
	struct hold *h;
	struct cg_server *server = view->server;

#ifdef SG_MUTANT_NO_HOLD
	return;
#endif
	if (!program_surface(view) || !view->scene_tree || !last_list_ms || now_ms() - last_list_ms > 3000) {
		return;   /* no taskbar framing windows now */
	}
	wl_list_for_each (h, &holds, link) {
		if (h->view == view) {
			return;
		}
	}
	if (!(h = calloc(1, sizeof(*h)))) {
		return;
	}
	h->view = view;
	h->timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->wl_display), hold_expired, h);
	if (!h->timer) {
		free(h);
		return;
	}
	wl_event_source_timer_update(h->timer, HOLD_MS);
	wl_list_insert(&holds, &h->link);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
}

void
session_x11_unhold(struct cg_view *view)
{
	struct hold *h, *tmp;
	wl_list_for_each_safe (h, tmp, &holds, link) {
		if (h->view == view) {
			end_hold(h, false);
		}
	}
}

bool
session_x11_held(struct cg_view *view)
{
	struct hold *h;
	wl_list_for_each (h, &holds, link) {
		if (h->view == view) {
			return true;
		}
	}
	return false;
}

size_t
session_x11_list(struct cg_server *server, char *buf, size_t len)
{
	struct cg_view *view, *focus = seat_get_focus(server->seat);
	struct wlr_xwayland_surface *xs;
	size_t off = 0;

	last_list_ms = now_ms();

	wl_list_for_each (view, &server->views, link) {
		char title[120], class[48];
		unsigned long id;
		int n;

		if ((xs = program_surface(view))) {
			copy_clean(title, sizeof(title), xs->title);
			copy_clean(class, sizeof(class), xs->class);
			id = xs->window_id;
		} else if (wayland_app_listed(view)) {
			/* a program's native Wayland window, under an id no X
			 * window has (wayland_app.c) */
			char *t = view_get_title(view);
			copy_clean(title, sizeof(title), t);
			free(t);
			copy_clean(class, sizeof(class), wayland_app_class(view));
			for (char *c = class; *c; c++) {
				if (*c == ' ') {
					*c = '_';
				}
			}
			id = wayland_app_window_id(view);
		} else {
			continue;
		}
		n = snprintf(buf + off, len - off, "%u %s %s %s\t%s\n", (unsigned) id,
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
	if (wayland_app_is(view)) {
		wayland_app_activate(view);
		return true;
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

/* The Windows key pressed while a Linux program's window has the keyboard:
 * the shell's keys (Start, Win+E, Win+R, Win+Tab...) are the shell's, as on
 * Windows -- they reached the Linux program and Start never opened (David
 * 2026-10-02 QA). The desktop comes in front with the focus first, so the key
 * reaches the shell. Not from an elevated window, the lock screen or a
 * consent prompt, which keep the keyboard. */
bool
session_x11_super(struct cg_server *server, struct cg_view *focus)
{
#ifdef SG_MUTANT_SUPER_STAYS
	return false;
#endif
	if (!focus || focus->elevated || lock_view_is_privileged(&server->lock, focus)) {
		return false;
	}
	if (focus->type == CAGE_XWAYLAND_VIEW && xwayland_view_is_shell_desktop(focus)) {
		return false;
	}
	if (!session_x11_desktop_front(server)) {
		return false;
	}
	/* X's input focus moves with a request on the window manager's own X
	 * connection, the key goes to Xwayland over Wayland: the request is
	 * made to arrive first (a round trip), else the key's press went to the
	 * window it left and only its release reached the shell -- no Start */
	xcb_connection_t *c = server->xwayland ? wlr_xwayland_get_xwm_connection(server->xwayland) : NULL;
#ifndef SG_MUTANT_SUPER_RACE
	if (c) {
		xcb_flush(c);
		free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	}
#else
	(void) c;
#endif
	return true;
}

bool
session_x11_minimize(struct cg_server *server, unsigned long window)
{
	struct cg_view *view = find(server, window);

	if (!view) {
		return false;
	}
	if (wayland_app_is(view)) {
		wayland_app_minimize(view);
		return true;
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

/* End task on a program that did not close when asked (a hung one: CPU-X,
 * David 2026-10-02): its process killed -- the one its window names
 * (_NET_WM_PID), when it is the asking user's own */
bool
session_x11_kill(struct cg_server *server, unsigned long window, uid_t uid)
{
	struct cg_view *view = find(server, window);
	struct wlr_xwayland_surface *xs;
	char proc[32];
	struct stat st;

	if (!view) {
		return false;
	}
	if (wayland_app_is(view)) {
#ifndef SG_MUTANT_XKILL_DEAF
		return wayland_app_kill(view, uid);
#else
		return true;
#endif
	}
	xs = xwayland_view_from_view(view)->xwayland_surface;
	snprintf(proc, sizeof(proc), "/proc/%d", (int) xs->pid);
	if (xs->pid <= 1 || xs->pid == getpid() || stat(proc, &st) || st.st_uid != uid) {
		return false;
	}
#ifndef SG_MUTANT_XKILL_DEAF
	return kill(xs->pid, SIGKILL) == 0;
#else
	return true;
#endif
}

bool
session_x11_close(struct cg_server *server, unsigned long window)
{
	struct cg_view *view = find(server, window);

	if (!view) {
		return false;
	}
	if (wayland_app_is(view)) {
		wayland_app_close(view);
		return true;
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
bool session_x11_super(struct cg_server *server, struct cg_view *focus) { (void) server; (void) focus; return false; }
bool session_x11_minimize(struct cg_server *server, unsigned long window) { (void) server; (void) window; return false; }
bool session_x11_close(struct cg_server *server, unsigned long window) { (void) server; (void) window; return false; }
bool session_x11_kill(struct cg_server *server, unsigned long window, uid_t uid) { (void) server; (void) window; (void) uid; return false; }
bool session_x11_desktop_front(struct cg_server *server) { (void) server; return false; }
void session_x11_hold(struct cg_view *view) { (void) view; }
void session_x11_unhold(struct cg_view *view) { (void) view; }
bool session_x11_held(struct cg_view *view) { (void) view; return false; }
#endif
