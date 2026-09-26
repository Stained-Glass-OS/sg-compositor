#ifndef CG_XWAYLAND_H
#define CG_XWAYLAND_H

#include <wayland-server-core.h>
#include <wlr/xwayland.h>

#include "view.h"

struct cg_xwayland_view {
	struct cg_view view;
	struct wlr_xwayland_surface *xwayland_surface;
	struct wl_listener destroy;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener unmap;
	struct wl_listener map;
	struct wl_listener request_fullscreen;

	/* sg-compositor: an elevated display's windows are managed windows --
	 * placed, moved, resized, minimised and maximised as they ask. */
	struct wl_listener request_configure;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_minimize;
	struct wl_listener request_maximize;
	struct wl_listener request_activate;
	struct wl_listener set_geometry;
};

struct cg_xwayland_view *xwayland_view_from_view(struct cg_view *view);
bool xwayland_view_should_manage(struct cg_view *view);
void handle_xwayland_surface_new(struct wl_listener *listener, void *data);
struct cg_elevated;
void xwayland_view_create(struct cg_server *server, struct wlr_xwayland_surface *xwayland_surface,
			  struct cg_elevated *elevated);

#endif
