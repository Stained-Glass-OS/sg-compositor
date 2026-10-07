#ifndef CG_XDG_SHELL_H
#define CG_XDG_SHELL_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "view.h"

struct cg_xdg_shell_view {
	struct cg_view view;
	struct wlr_xdg_toplevel *xdg_toplevel;

	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener unmap;
	struct wl_listener map;
	struct wl_listener request_fullscreen;

	/* sg-compositor: an ordinary program's window, managed like the
	 * session's other windows (wayland_app.c) */
	bool managed;
	unsigned long window_id; /* its id on the taskbar's list */
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_minimize;
	bool fullscreen;
	bool want_maximized, want_fullscreen; /* asked before it mapped */
	struct wlr_box windowed;              /* where it was before full screen */
	int last_w, last_h;                   /* the size last placed */
	int gx, gy;                           /* its geometry's offset when placed */
};

struct cg_xdg_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration;
	struct cg_server *server;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_mode;
};

struct cg_xdg_popup {
	struct wlr_xdg_popup *xdg_popup;

	struct wl_listener destroy;
	struct wl_listener commit;
};

void handle_new_xdg_toplevel(struct wl_listener *listener, void *data);
void handle_new_xdg_popup(struct wl_listener *listener, void *data);

void handle_xdg_toplevel_decoration(struct wl_listener *listener, void *data);

#endif
