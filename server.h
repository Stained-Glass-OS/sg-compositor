#ifndef CG_SERVER_H
#define CG_SERVER_H

#include "config.h"
#include "lock.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

enum cg_multi_output_mode {
	CAGE_MULTI_OUTPUT_MODE_EXTEND,
	CAGE_MULTI_OUTPUT_MODE_LAST,
};

struct cg_server {
	struct wl_display *wl_display;
	struct wl_list views;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_session *session;
	struct wl_listener display_destroy;

	struct cg_seat *seat;
	struct wlr_idle_notifier_v1 *idle;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_v1;
	struct wl_listener new_idle_inhibitor_v1;
	struct wl_list inhibitors;
	/* Control-socket connections holding the idle off (INHIBIT): the
	 * session's org.freedesktop.ScreenSaver service, for programs that are
	 * not Wayland clients (Wine's power requests, X11 programs). */
	int control_inhibitors;

	enum cg_multi_output_mode output_mode;
	struct wlr_output_layout *output_layout;
	struct wlr_scene_output_layout *scene_output_layout;

	struct wlr_scene *scene;
	/* Includes disabled outputs; depending on the output_mode
	 * some outputs may be disabled. */
	struct wl_list outputs; // cg_output::link
	struct wl_listener new_output;
	struct wl_listener output_layout_change;

	struct wl_listener xdg_toplevel_decoration;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;

	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;
#if CAGE_HAS_XWAYLAND
	struct wl_listener new_xwayland_surface;
#endif
	struct wlr_output_manager_v1 *output_manager_v1;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener output_power_set_mode; /* sg-compositor: display power */

	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;

	bool xdg_decoration;
	bool allow_vt_switch;
	bool return_app_code;
	bool terminated;

	/* sg-compositor: lock mode and privileged clients, see lock.c */
	struct cg_lock lock;
	const char *lock_socket;
	const char *control_socket;
	const char *lock_uid;

	/* sg-compositor: Remote Desktop takes over this (console) session -- the
	 * user's windows move to an output of their own that the RDP daemon
	 * captures, and the console goes dark and deaf. See remote_attach(). */
	struct wlr_backend *remote_backend; /* headless, part of the multi backend */
	struct wlr_output *remote_output;
	bool remote;
	/* The RDP daemon's connection, handed over with REMOTE: when it closes,
	 * the session goes back to the console. */
	struct wl_client *remote_client;
	struct wl_listener remote_client_destroy;

	/* sg-compositor: console shadow (E1 pattern A) -- Remote Desktop views,
	 * or views and controls, this session where it is, at the console,
	 * which keeps working as before. The RDP daemon's connection, handed
	 * over with SHADOW; while it lasts a frame round the screen says so,
	 * and when it closes nothing else changes. See shadow_attach(). */
	struct wl_client *shadow_client;
	struct wl_listener shadow_client_destroy;
	bool shadow_control; /* may it type and click (else view only) */
	struct wlr_scene_tree *shadow_frame_tree; /* the frame, above everything */
	struct wlr_scene_rect *shadow_frame[4];

	/* sg-compositor: elevated programs' displays (elevated.c). Stacking, from
	 * the bottom: the session's views, elevated programs' windows -- which no
	 * session window can cover -- and privileged views (lock screen, consent
	 * prompt) on top of everything. */
	struct wl_list elevated; /* cg_elevated::link */
	struct wlr_scene_tree *backdrop_tree; /* beneath everything: backdrop.c */
	struct wlr_scene_tree *normal_tree;
	struct wlr_scene_tree *elevated_tree;
	struct wlr_scene_tree *privileged_tree;
	/* sg-compositor: a secure prompt's dimming, over the desktop and under
	 * the prompt (lock.c) */
	struct wlr_scene_rect *dim_rect;
	struct wlr_compositor *compositor;
#if CAGE_HAS_XWAYLAND
	struct wlr_xwayland *xwayland; /* the session's own */
#endif
};

void server_terminate(struct cg_server *server);

#endif
