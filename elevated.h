/*
 * sg-compositor: elevated programs' displays (ADR 0012, bug B56).
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef CG_ELEVATED_H
#define CG_ELEVATED_H

#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>

struct cg_server;
struct cg_view;
struct cg_seat;

/* One X server per elevated program. The X server runs as the SYSTEM account
 * (started by the elevation broker after consent), never as the session's
 * user, and this compositor is its window manager: its windows are composited
 * into the user's desktop as ordinary windows, above the session's own. */
struct cg_elevated {
	struct wl_list link; /* cg_server::elevated */
	struct cg_server *server;
	int id;	   /* its X display number, once it is ready; -1 before */
	uid_t uid; /* who handed it over (the SYSTEM account or root) */

	struct wl_client *client; /* the elevated Xwayland */
	struct wl_listener client_destroy;

	/* A wlr_xwayland_server we built ourselves: wlroots' own would spawn
	 * Xwayland as this compositor's user, which is the one account that
	 * must not own an elevated display. */
	struct wlr_xwayland_server *xserver;
	struct wlr_xwayland *xwayland;
	struct wlr_xwayland_shell_v1 *shell;
	struct wl_listener new_surface;

	/* This display's clipboard, and nothing else: a seat no client can see.
	 * Its xwm keeps the X CLIPBOARD in step with it; what crosses to and from
	 * the session's own seat is decided in elevated.c. */
	struct wlr_seat *clip_seat;
	struct wl_listener clip_request_set_selection;
	struct wl_listener clip_request_set_primary_selection;
	struct wl_listener clip_request_start_drag;

	int ready_fd;
	struct wl_event_source *ready_source;
	char ready_buf[16];
	size_t ready_len;
};

void elevated_init(struct cg_server *server);

/* ELEVATED on the control socket: the broker's ends of the new X server's
 * Wayland connection, its window-manager connection, and a pipe on which the
 * display number arrives once the X server is ready. */
bool elevated_adopt(struct cg_server *server, uid_t uid, int wl_fd, int wm_fd, int ready_fd);

/* ACTIVATE <display> <window>: the taskbar or the switcher brings an elevated
 * window forward (and back from minimised). Not input: nothing is typed or
 * clicked into it. */
bool elevated_activate(struct cg_server *server, int id, unsigned long window);

/* Global filter: 1 visible, 0 hidden, -1 not an elevated display's concern. */
int elevated_filter_global(struct cg_server *server, const struct wl_client *client, const struct wl_global *global);

size_t elevated_list(struct cg_server *server, char *buf, size_t len);

bool elevated_client(struct cg_server *server, const struct wl_client *client);

/* The user (physical input, or remote access's privileged virtual devices --
 * never a session program) pressed a key or a button on an elevated view.
 * That, and only that, offers the session's clipboard text to it. */
void elevated_user_input(struct cg_view *view);

/* Views of elevated displays: where they go and how the window manager's
 * requests are answered. */
void elevated_view_mapped(struct cg_view *view);
void elevated_view_place(struct cg_view *view);

void elevated_view_focused(struct cg_view *view);
void elevated_focus_session(struct cg_server *server);

struct cg_xwayland_view;
void elevated_view_listen(struct cg_xwayland_view *xv);
void elevated_view_unlisten(struct cg_xwayland_view *xv);

/* Interactive move and resize (_NET_WM_MOVERESIZE) of an elevated window. */
bool elevated_grab_motion(struct cg_seat *seat, double lx, double ly);
bool elevated_grab_button(struct cg_seat *seat, bool pressed);

#endif
