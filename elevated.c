/*
 * sg-compositor: elevated programs' displays (ADR 0012, bug B56).
 *
 * An elevated program runs as the SYSTEM account, a different Unix account
 * from the session's, so the kernel keeps the session's programs out of its
 * memory. Its *display* is the other channel: on the session's own X server
 * any session program could type into it (XTEST, XSendEvent) or photograph it
 * (XGetImage) -- the hole Windows closes with UIPI. So every elevated program
 * gets an X server of its own, started by the elevation broker as the SYSTEM
 * account, and this compositor is its window manager:
 *
 *   - The broker hands over the new X server's Wayland connection, its window
 *     manager connection and a readiness pipe (ELEVATED on the control
 *     socket, the SYSTEM account or root only, by SO_PEERCRED). The X server
 *     is never this compositor's child: the compositor runs as the session's
 *     user, the one account that must not own an elevated display.
 *   - Its windows are composited into the desktop as ordinary windows: placed
 *     where they ask, moved and resized by their title bars and borders
 *     (_NET_WM_MOVERESIZE), minimised, maximised, focused by a click. They
 *     stack above every session window, so a session program can neither
 *     cover nor imitate an elevated window's place on screen, and below the
 *     lock screen and consent prompt. While the machine is locked they are
 *     hidden and get no input, like any session window.
 *   - The user's keyboard and pointer reach them through this compositor. No
 *     session program can: they are not on the session's X server, their X
 *     server admits only the SYSTEM account, and input injection and screen
 *     capture over Wayland are privileged (lock.c).
 *
 * Clipboard and drag and drop follow UIPI's rule that a less trusted program
 * must not drive an elevated one:
 *
 *   - Elevated to session: what an elevated program copies is offered to the
 *     session (text only). That is data leaving the elevated program by its
 *     own choice, which Windows allows too.
 *   - Session to elevated: the session's clipboard text is offered to an
 *     elevated display only when the user presses a key or a button on one of
 *     its windows -- input only the compositor can produce -- so nothing the
 *     session does on its own pushes data into an elevated program, and what
 *     arrives is what the user was about to paste. Text only: no files, no
 *     images, no formats with parsers of their own.
 *   - Drag and drop: none, in either direction. Windows' UIPI refuses a drop
 *     from a lower integrity level; here the elevated display's clipboard seat
 *     never carries a drag, and elevated X servers are not offered the data
 *     device protocol.
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "elevated.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/shell.h>

#include "seat.h"
#include "server.h"
#include "view.h"
#include "xwayland.h"

#define AUDIT(...) wlr_log(WLR_ERROR, "audit: " __VA_ARGS__)

void
elevated_init(struct cg_server *server)
{
	wl_list_init(&server->elevated);
}

/* ---- clipboard ----------------------------------------------------------
 * A proxy data source stands between two seats: the session's and an
 * elevated display's hidden clipboard seat. It offers only text, forwards a
 * paste to the source it wraps, and disappears with it. It also hides the
 * wrapped source's kind: wlroots' window manager ignores a selection that
 * came from any Xwayland, so without it no text would cross at all. */
struct cg_clip_proxy {
	struct wlr_data_source base;
	struct wlr_data_source *inner;
	struct wl_listener inner_destroy;
	struct wlr_seat *target;
	struct cg_server *server;
};

static const char *const text_types[] = {
	"text/plain;charset=utf-8", "text/plain;charset=UTF-8", "text/plain", "UTF8_STRING", "STRING", "TEXT", NULL,
};

static bool
is_text_type(const char *mime)
{
	for (int i = 0; text_types[i]; i++) {
		if (!strcmp(mime, text_types[i])) {
			return true;
		}
	}
	return false;
}

static void
proxy_send(struct wlr_data_source *source, const char *mime_type, int32_t fd)
{
	struct cg_clip_proxy *p = wl_container_of(source, p, base);
	if (p->inner && is_text_type(mime_type)) {
		wlr_data_source_send(p->inner, mime_type, fd); /* takes the fd */
	} else {
		close(fd);
	}
}

static void
proxy_destroy(struct wlr_data_source *source)
{
	struct cg_clip_proxy *p = wl_container_of(source, p, base);
	if (p->inner) {
		wl_list_remove(&p->inner_destroy.link);
	}
	free(p);
}

static const struct wlr_data_source_impl proxy_impl = {
	.send = proxy_send,
	.destroy = proxy_destroy,
};

static struct cg_clip_proxy *
as_proxy(struct wlr_data_source *source)
{
	return source && source->impl == &proxy_impl ? wl_container_of(source, (struct cg_clip_proxy *) NULL, base)
						     : NULL;
}

static void
proxy_handle_inner_destroy(struct wl_listener *listener, void *data)
{
	struct cg_clip_proxy *p = wl_container_of(listener, p, inner_destroy);
	(void) data;
	wl_list_remove(&p->inner_destroy.link);
	p->inner = NULL;
	if (p->target->selection_source == &p->base) {
		/* frees p */
		wlr_seat_set_selection(p->target, NULL, wl_display_next_serial(p->server->wl_display));
	}
}

static struct cg_clip_proxy *
proxy_create(struct cg_server *server, struct wlr_data_source *inner, struct wlr_seat *target)
{
	struct cg_clip_proxy *p = calloc(1, sizeof(*p));
	char **mime;
	int n = 0;

	if (!p) {
		return NULL;
	}
	wlr_data_source_init(&p->base, &proxy_impl);
	wl_array_for_each (mime, &inner->mime_types) {
		char **copy;
		if (!is_text_type(*mime) || !(copy = wl_array_add(&p->base.mime_types, sizeof(*copy)))) {
			continue;
		}
		if (!(*copy = strdup(*mime))) {
			p->base.mime_types.size -= sizeof(*copy);
			continue;
		}
		n++;
	}
	if (!n) {
		wlr_data_source_destroy(&p->base); /* nothing that may cross */
		return NULL;
	}
	p->inner = inner;
	p->target = target;
	p->server = server;
	p->inner_destroy.notify = proxy_handle_inner_destroy;
	wl_signal_add(&inner->events.destroy, &p->inner_destroy);
	return p;
}

/* The elevated program copied something: its display's window manager sets
 * it on the hidden seat, and the session is offered the text. */
static void
handle_clip_request_set_selection(struct wl_listener *listener, void *data)
{
	struct cg_elevated *e = wl_container_of(listener, e, clip_request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	struct cg_server *server = e->server;
	struct cg_clip_proxy *p;

	wlr_seat_set_selection(e->clip_seat, event->source, event->serial);
	if (!event->source || as_proxy(event->source)) {
		return;
	}
	if ((p = proxy_create(server, event->source, server->seat->seat))) {
		wlr_seat_set_selection(server->seat->seat, &p->base, wl_display_next_serial(server->wl_display));
		wlr_log(WLR_INFO, "elevated display :%d: its copied text is offered to the session", e->id);
	}
}

/* Nothing but CLIPBOARD crosses: no primary selection, no drags. */
static void
handle_clip_request_set_primary_selection(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_set_primary_selection_event *event = data;
	(void) listener;
	if (event->source) {
		wlr_primary_selection_source_destroy(event->source);
	}
}

static void
handle_clip_request_start_drag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;
	(void) listener;
	wlr_data_source_destroy(event->drag->source);
}

void
elevated_user_input(struct cg_view *view)
{
	struct cg_elevated *e = view ? view->elevated : NULL;
	struct wlr_data_source *src, *cur;
	struct cg_clip_proxy *p;

	if (!e || !e->clip_seat) {
		return;
	}
	src = e->server->seat->seat->selection_source;
	cur = e->clip_seat->selection_source;
	if (!src) {
		return;
	}
	/* Already there: offered before, or the text came from this display. */
	if ((p = as_proxy(cur)) && p->inner == src) {
		return;
	}
	if ((p = as_proxy(src)) && p->inner == cur) {
		return;
	}
	if (!(p = proxy_create(e->server, src, e->clip_seat))) {
		return;
	}
	wlr_seat_set_selection(e->clip_seat, &p->base, wl_display_next_serial(e->server->wl_display));
	wlr_log(WLR_INFO, "elevated display :%d: the session's clipboard text offered on the user's input", e->id);
}

/* ---- globals -------------------------------------------------------------
 * Each Xwayland binds the xwayland_shell_v1 global it finds, and wlroots
 * kills a client that binds a shell not meant for it: every X server may see
 * its own shell only. The hidden clipboard seats are no client's business.
 * An elevated X server gets no data device (no drag and drop; the clipboard
 * is its window manager's). */
int
elevated_filter_global(struct cg_server *server, const struct wl_client *client, const struct wl_global *global)
{
	struct cg_elevated *e;
	const char *name = wl_global_get_interface(global)->name;

	wl_list_for_each (e, &server->elevated, link) {
		if (e->shell && global == e->shell->global) {
			return client == e->client;
		}
		if (e->clip_seat && global == e->clip_seat->global) {
			return 0;
		}
	}
#if CAGE_HAS_XWAYLAND
	if (server->xwayland && server->xwayland->shell_v1 && global == server->xwayland->shell_v1->global) {
		return server->xwayland->server && client == server->xwayland->server->client;
	}
#endif
	/* A shell or a seat being created right now is announced to every
	 * client before elevated_adopt() has recorded it: hidden until it is. */
	if (!strcmp(name, "xwayland_shell_v1")) {
		return 0;
	}
	if (!strcmp(name, "wl_seat") && global != server->seat->seat->global) {
		return 0;
	}
	if (elevated_client(server, client) &&
	    (!strcmp(name, "wl_data_device_manager") || !strcmp(name, "zwp_primary_selection_device_manager_v1"))) {
		return 0;
	}
	return -1;
}

bool
elevated_client(struct cg_server *server, const struct wl_client *client)
{
	struct cg_elevated *e;
	wl_list_for_each (e, &server->elevated, link) {
		if (e->client == client) {
			return true;
		}
	}
	return false;
}

/* ---- life cycle ---------------------------------------------------------- */
static void
elevated_destroy(struct cg_elevated *e)
{
	if (e->ready_source) {
		wl_event_source_remove(e->ready_source);
	}
	if (e->ready_fd >= 0) {
		close(e->ready_fd);
	}
	if (e->xwayland) {
		wl_list_remove(&e->new_surface.link);
		/* wlroots' window manager destroys itself when its connection
		 * hangs up, dropping the seat on the way, but leaves its pointer
		 * behind: without this the next line would free it twice. */
		if (!e->xwayland->seat) {
			e->xwayland->xwm = NULL;
		}
		wlr_xwayland_destroy(e->xwayland); /* and the shell with it */
	} else if (e->shell) {
		wlr_xwayland_shell_v1_destroy(e->shell);
	}
	free(e->xserver);
	if (e->clip_seat) {
		wl_list_remove(&e->clip_request_set_selection.link);
		wl_list_remove(&e->clip_request_set_primary_selection.link);
		wl_list_remove(&e->clip_request_start_drag.link);
		wlr_seat_destroy(e->clip_seat);
	}
	wl_list_remove(&e->link);
	AUDIT("elevated display :%d closed", e->id);
	free(e);
}

static void
handle_client_destroy(struct wl_listener *listener, void *data)
{
	struct cg_elevated *e = wl_container_of(listener, e, client_destroy);
	(void) data;
	wl_list_remove(&e->client_destroy.link);
	e->client = NULL;
	elevated_destroy(e);
}

static void
handle_new_surface(struct wl_listener *listener, void *data)
{
	struct cg_elevated *e = wl_container_of(listener, e, new_surface);
	xwayland_view_create(e->server, data, e);
}

/* The broker writes the display number once Xwayland has said it is ready;
 * only then may the window manager connect (xcb waits for the X server's
 * reply, and the X server first waits for us). */
static int
handle_ready(int fd, uint32_t mask, void *data)
{
	struct cg_elevated *e = data;
	char *end;
	long id;

	if (mask & WL_EVENT_READABLE) {
		ssize_t n = read(fd, e->ready_buf + e->ready_len, sizeof(e->ready_buf) - 1 - e->ready_len);
		if (n > 0) {
			e->ready_len += (size_t) n;
			e->ready_buf[e->ready_len] = 0;
			if (!strchr(e->ready_buf, '\n') && e->ready_len < sizeof(e->ready_buf) - 1) {
				return 0;
			}
		}
	}
	wl_event_source_remove(e->ready_source);
	e->ready_source = NULL;
	close(e->ready_fd);
	e->ready_fd = -1;

	errno = 0;
	id = strtol(e->ready_buf, &end, 10);
	if (!strchr(e->ready_buf, '\n') || end == e->ready_buf || *end != '\n' || errno || id < 0 || id > 65535) {
		AUDIT("elevated display from uid %d never became ready", (int) e->uid);
		if (e->client) {
			wl_client_destroy(e->client); /* tears down e */
		}
		return 0;
	}
	e->id = (int) id;
	e->xserver->display = e->id;
	snprintf(e->xserver->display_name, sizeof(e->xserver->display_name), ":%d", e->id);
	e->xserver->ready = true;
	wl_signal_emit_mutable(&e->xserver->events.ready, e->xserver);
	if (!e->xwayland->xwm) {
		AUDIT("elevated display :%d: the window manager could not connect", e->id);
		if (e->client) {
			wl_client_destroy(e->client);
		}
		return 0;
	}
	AUDIT("elevated display :%d ready (uid %d)", e->id, (int) e->uid);
	return 0;
}

bool
elevated_adopt(struct cg_server *server, uid_t uid, int wl_fd, int wm_fd, int ready_fd)
{
	struct cg_elevated *e = calloc(1, sizeof(*e));
	struct wlr_xwayland_server *xs;

	if (!e) {
		goto fail_fds;
	}
	e->server = server;
	e->id = -1;
	e->uid = uid;
	e->ready_fd = -1;
	wl_list_insert(&server->elevated, &e->link);

	if (!(e->client = wl_client_create(server->wl_display, wl_fd))) {
		wl_list_remove(&e->link);
		free(e);
		goto fail_fds;
	}
	e->client_destroy.notify = handle_client_destroy;
	wl_client_add_destroy_listener(e->client, &e->client_destroy);
	e->ready_fd = ready_fd;

	if (!(e->shell = wlr_xwayland_shell_v1_create(server->wl_display, 1)) ||
	    !(e->clip_seat = wlr_seat_create(server->wl_display, "sg-elevated-clipboard")) ||
	    !(e->xserver = xs = calloc(1, sizeof(*xs)))) {
		close(wm_fd);
		wl_client_destroy(e->client);
		return false;
	}
	e->clip_request_set_selection.notify = handle_clip_request_set_selection;
	wl_signal_add(&e->clip_seat->events.request_set_selection, &e->clip_request_set_selection);
	e->clip_request_set_primary_selection.notify = handle_clip_request_set_primary_selection;
	wl_signal_add(&e->clip_seat->events.request_set_primary_selection, &e->clip_request_set_primary_selection);
	e->clip_request_start_drag.notify = handle_clip_request_start_drag;
	wl_signal_add(&e->clip_seat->events.request_start_drag, &e->clip_request_start_drag);

	/* A server wlroots did not start: never spawned, never restarted. */
	xs->client = e->client;
	xs->wm_fd[0] = wm_fd;
	xs->wm_fd[1] = xs->wl_fd[0] = xs->wl_fd[1] = xs->x_fd[0] = xs->x_fd[1] = -1;
	xs->display = -1;
	snprintf(xs->display_name, sizeof(xs->display_name), ":elevated");
	xs->options.enable_wm = true;
	xs->wl_display = server->wl_display;
	wl_signal_init(&xs->events.start);
	wl_signal_init(&xs->events.ready);
	wl_signal_init(&xs->events.destroy);
	wl_list_init(&xs->client_destroy.link);
	wl_list_init(&xs->display_destroy.link);

	if (!(e->xwayland = wlr_xwayland_create_with_server(server->wl_display, server->compositor, xs))) {
		close(wm_fd);
		xs->wm_fd[0] = -1;
		wl_client_destroy(e->client);
		return false;
	}
	e->xwayland->shell_v1 = e->shell;
	wlr_xwayland_shell_v1_set_client(e->shell, e->client);
	wlr_xwayland_set_seat(e->xwayland, e->clip_seat);
	e->new_surface.notify = handle_new_surface;
	wl_signal_add(&e->xwayland->events.new_surface, &e->new_surface);

	e->ready_source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), ready_fd,
					       WL_EVENT_READABLE, handle_ready, e);
	if (!e->ready_source) {
		wl_client_destroy(e->client);
		return false;
	}
	AUDIT("elevated display accepted from uid %d", (int) uid);
	return true;

fail_fds:
	close(wl_fd);
	close(wm_fd);
	close(ready_fd);
	return false;
}

/* ---- windows ------------------------------------------------------------- */
static struct wlr_xwayland_surface *
xsurface_of(struct cg_view *view)
{
	return xwayland_view_from_view(view)->xwayland_surface;
}

static void
view_move_to(struct cg_view *view, int x, int y)
{
	struct wlr_box box;
	view_get_layout_box(view, &box);
	view->lx = x;
	view->ly = y;
	view->rx = x - box.x;
	view->ry = y - box.y;
	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	}
}

/* Raise a window and, above it, its own dialogs and menus. */
static void
elevated_raise(struct cg_view *view)
{
	struct cg_view *other;
	struct wlr_xwayland_surface *xs = xsurface_of(view);

	if (!view->scene_tree) {
		return;
	}
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	wl_list_for_each_reverse (other, &view->server->views, link) {
		struct wlr_xwayland_surface *p;
		if (other == view || other->elevated != view->elevated || !other->scene_tree) {
			continue;
		}
		for (p = xsurface_of(other)->parent; p; p = p->parent) {
			if (p == xs) {
				wlr_scene_node_raise_to_top(&other->scene_tree->node);
				break;
			}
		}
	}
}

static bool
view_visible(struct cg_view *view)
{
	return view->scene_tree && view->scene_tree->node.enabled && !view->minimized;
}

void
elevated_focus_session(struct cg_server *server)
{
	struct cg_view *view;
	wl_list_for_each (view, &server->views, link) {
		if (!view->elevated && !lock_view_is_privileged(&server->lock, view) && view_visible(view)) {
			seat_set_focus(server->seat, view);
			return;
		}
	}
}

void
elevated_view_focused(struct cg_view *view)
{
	elevated_raise(view);
}

void
elevated_view_mapped(struct cg_view *view)
{
	struct wlr_xwayland_surface *xs = xsurface_of(view);
	struct wlr_box box;
	int x = xs->x, y = xs->y;

	view_get_layout_box(view, &box);
	if (!xs->override_redirect &&
	    (x < box.x || y < box.y || x > box.x + box.width - 48 || y > box.y + box.height - 32)) {
		/* A managed window that would open with its title bar out of
		 * reach is centred instead. */
		x = box.x + (box.width - xs->width) / 2;
		y = box.y + (box.height - xs->height) / 2;
		x = x < box.x ? box.x : x;
		y = y < box.y ? box.y : y;
		wlr_xwayland_surface_configure(xs, x, y, xs->width, xs->height);
	}
	view_move_to(view, x, y);
	elevated_raise(view);
	wlr_log(WLR_INFO, "elevated display :%d: window 0x%x mapped at %d,%d %dx%d", view->elevated->id,
		(unsigned) xs->window_id, x, y, xs->width, xs->height);
}

/* The layout changed, or Remote Desktop took the session or gave it back:
 * keep each window where it was relative to the session's desktop. */
void
elevated_view_place(struct cg_view *view)
{
	struct wlr_xwayland_surface *xs = xsurface_of(view);
	struct wlr_box box;

	view_get_layout_box(view, &box);
	if (view->maximized) {
		wlr_xwayland_surface_configure(xs, box.x, box.y, box.width, box.height);
		view_move_to(view, box.x, box.y);
		return;
	}
	if (!xs->override_redirect) {
		wlr_xwayland_surface_configure(xs, box.x + view->rx, box.y + view->ry, xs->width, xs->height);
	}
	view_move_to(view, box.x + view->rx, box.y + view->ry);
}

static void
view_restore(struct cg_view *view)
{
	struct cg_server *server = view->server;
	if (!view->minimized) {
		return;
	}
	view->minimized = false;
	wlr_xwayland_surface_set_minimized(xsurface_of(view), false);
	if (view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, lock_view_allowed(&server->lock, view));
	}
}

bool
elevated_activate(struct cg_server *server, int id, unsigned long window)
{
	struct cg_view *view;
	wl_list_for_each (view, &server->views, link) {
		if (view->elevated && view->elevated->id == id && xsurface_of(view)->window_id == window) {
			view_restore(view);
			seat_set_focus(server->seat, view);
			elevated_raise(view);
			return true;
		}
	}
	return false;
}

/* ---- the window manager's requests -------------------------------------- */
static void
handle_request_configure(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xv = wl_container_of(listener, xv, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	struct cg_view *view = &xv->view;

	if (view->maximized) {
		/* stays maximised until it asks otherwise */
		wlr_xwayland_surface_configure(ev->surface, ev->surface->x, ev->surface->y, ev->surface->width,
					       ev->surface->height);
		return;
	}
	wlr_xwayland_surface_configure(ev->surface, ev->x, ev->y, ev->width < 1 ? 1 : ev->width,
				       ev->height < 1 ? 1 : ev->height);
	if (view->wlr_surface) {
		view_move_to(view, ev->x, ev->y);
	}
}

static void
handle_set_geometry(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xv = wl_container_of(listener, xv, set_geometry);
	(void) data;
	if (xv->view.wlr_surface) {
		view_move_to(&xv->view, xv->xwayland_surface->x, xv->xwayland_surface->y);
	}
}

static void
begin_grab(struct cg_view *view, bool resize, uint32_t edges)
{
	struct cg_seat *seat = view->server->seat;
	struct wlr_xwayland_surface *xs = xsurface_of(view);

	if (!view->wlr_surface || view->maximized || seat->seat->pointer_state.button_count == 0 ||
	    seat_get_focus(seat) != view) {
		return; /* only the focused window, and only while the user holds a button */
	}
	seat->grab_view = view;
	seat->grab_resize = resize;
	seat->grab_edges = edges;
	seat->grab_cx = seat->cursor->x;
	seat->grab_cy = seat->cursor->y;
	seat->grab_box = (struct wlr_box){view->lx, view->ly, xs->width, xs->height};
}

static void
handle_request_move(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xv = wl_container_of(listener, xv, request_move);
	(void) data;
	begin_grab(&xv->view, false, 0);
}

static void
handle_request_resize(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xv = wl_container_of(listener, xv, request_resize);
	struct wlr_xwayland_resize_event *ev = data;
	begin_grab(&xv->view, true, ev->edges);
}

bool
elevated_grab_motion(struct cg_seat *seat, double lx, double ly)
{
	struct cg_view *view = seat->grab_view;
	struct wlr_box b = seat->grab_box;
	int dx, dy;

	if (!view) {
		return false;
	}
	if (seat->seat->pointer_state.button_count == 0 || !view->wlr_surface) {
		seat->grab_view = NULL;
		return false;
	}
	dx = (int) (lx - seat->grab_cx);
	dy = (int) (ly - seat->grab_cy);
	if (!seat->grab_resize) {
		b.x += dx;
		b.y += dy;
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
		if (b.width < 100) {
			b.x -= (seat->grab_edges & WLR_EDGE_LEFT) ? 100 - b.width : 0;
			b.width = 100;
		}
		if (b.height < 40) {
			b.y -= (seat->grab_edges & WLR_EDGE_TOP) ? 40 - b.height : 0;
			b.height = 40;
		}
	}
	wlr_xwayland_surface_configure(xsurface_of(view), b.x, b.y, b.width, b.height);
	view_move_to(view, b.x, b.y);
	return true;
}

bool
elevated_grab_button(struct cg_seat *seat, bool pressed)
{
	if (seat->grab_view && !pressed) {
		seat->grab_view = NULL; /* the release still goes to the window */
	}
	return false;
}

static void
handle_request_minimize(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xv = wl_container_of(listener, xv, request_minimize);
	struct wlr_xwayland_minimize_event *ev = data;
	struct cg_view *view = &xv->view;
	struct cg_server *server = view->server;

	if (!ev->minimize) {
		view_restore(view);
		return;
	}
	if (view->minimized || !view->scene_tree) {
		return;
	}
	view->minimized = true;
	wlr_xwayland_surface_set_minimized(xv->xwayland_surface, true);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (server->seat->grab_view == view) {
		server->seat->grab_view = NULL;
	}
	if (seat_get_focus(server->seat) == view) {
		elevated_focus_session(server);
	}
}

static void
handle_request_maximize(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xv = wl_container_of(listener, xv, request_maximize);
	struct wlr_xwayland_surface *xs = xv->xwayland_surface;
	struct cg_view *view = &xv->view;
	bool want = xs->maximized_vert && xs->maximized_horz;
	struct wlr_box box;
	(void) data;

	if (want == view->maximized) {
		return;
	}
	view_get_layout_box(view, &box);
	if (want) {
		view->restore = (struct wlr_box){view->lx, view->ly, xs->width, xs->height};
		view->maximized = true;
		wlr_xwayland_surface_configure(xs, box.x, box.y, box.width, box.height);
		wlr_xwayland_surface_set_maximized(xs, true);
		view_move_to(view, box.x, box.y);
	} else {
		view->maximized = false;
		wlr_xwayland_surface_configure(xs, view->restore.x, view->restore.y, view->restore.width,
					       view->restore.height);
		wlr_xwayland_surface_set_maximized(xs, false);
		view_move_to(view, view->restore.x, view->restore.y);
	}
}

static void
handle_request_activate(struct wl_listener *listener, void *data)
{
	struct cg_xwayland_view *xv = wl_container_of(listener, xv, request_activate);
	(void) data;
	if (xv->view.wlr_surface) {
		view_restore(&xv->view);
		seat_set_focus(xv->view.server->seat, &xv->view);
	}
}

void
elevated_view_listen(struct cg_xwayland_view *xv)
{
	struct wlr_xwayland_surface *xs = xv->xwayland_surface;

	xv->request_configure.notify = handle_request_configure;
	wl_signal_add(&xs->events.request_configure, &xv->request_configure);
	xv->set_geometry.notify = handle_set_geometry;
	wl_signal_add(&xs->events.set_geometry, &xv->set_geometry);
	xv->request_move.notify = handle_request_move;
	wl_signal_add(&xs->events.request_move, &xv->request_move);
	xv->request_resize.notify = handle_request_resize;
	wl_signal_add(&xs->events.request_resize, &xv->request_resize);
	xv->request_minimize.notify = handle_request_minimize;
	wl_signal_add(&xs->events.request_minimize, &xv->request_minimize);
	xv->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&xs->events.request_maximize, &xv->request_maximize);
	xv->request_activate.notify = handle_request_activate;
	wl_signal_add(&xs->events.request_activate, &xv->request_activate);
}

void
elevated_view_unlisten(struct cg_xwayland_view *xv)
{
	wl_list_remove(&xv->request_configure.link);
	wl_list_remove(&xv->set_geometry.link);
	wl_list_remove(&xv->request_move.link);
	wl_list_remove(&xv->request_resize.link);
	wl_list_remove(&xv->request_minimize.link);
	wl_list_remove(&xv->request_maximize.link);
	wl_list_remove(&xv->request_activate.link);
}

/* WINDOWS on the control socket: the elevated windows, for the taskbar and
 * the gates. One line each -- "<display> <window> <x> <y> <w> <h>
 * shown|minimized|hidden focused|- <title>" -- then "END". A window's title
 * is not a secret from the session on Windows either (GetWindowText). */
size_t
elevated_list(struct cg_server *server, char *buf, size_t len)
{
	struct cg_view *view, *focus = seat_get_focus(server->seat);
	size_t off = 0;

	wl_list_for_each (view, &server->views, link) {
		struct wlr_xwayland_surface *xs;
		char title[80];
		int i = 0;
		if (!view->elevated || (xs = xsurface_of(view))->override_redirect) {
			continue;
		}
		for (const char *t = xs->title ? xs->title : ""; *t && i < (int) sizeof(title) - 1; t++) {
			title[i++] = (unsigned char) *t < 0x20 ? '?' : *t;
		}
		title[i] = 0;
		int n = snprintf(buf + off, len - off, "%d %u %d %d %d %d %s %s %s\n", view->elevated->id,
				 (unsigned) xs->window_id, view->lx, view->ly, xs->width, xs->height,
				 view->minimized	      ? "minimized"
				 : view_visible(view) ? "shown"
						      : "hidden",
				 view == focus ? "focused" : "-", title);
		if (n < 0 || (size_t) n >= len - off - 5) {
			break;
		}
		off += (size_t) n;
	}
	off += (size_t) snprintf(buf + off, len - off, "END\n");
	return off;
}
