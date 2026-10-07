#ifndef CG_SEAT_H
#define CG_SEAT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_seat.h>
#include <time.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include "server.h"
#include "view.h"

#define DEFAULT_XCURSOR "left_ptr"
#define XCURSOR_SIZE 24

struct cg_seat {
	/* sg-compositor: a key left out of the keys held when focus moves (the
	 * shell's key that moved it: its press must reach the new window) */
	uint32_t enter_skip_keycode;
	/* ...whose press is held until the next key event, so it reaches the
	 * new window after X's focus has moved there (0: none) */
	uint32_t held_press_keycode;
	struct wlr_seat *seat;
	struct cg_server *server;
	struct wl_listener destroy;

	struct wl_list keyboards;
	struct wl_list keyboard_groups;
	struct wl_list pointers;
	struct wl_list touch;
	struct wl_list tablets; /* sg-compositor: pens, as pointers */
	/* sg-compositor: pens to programs as pens (pressure, tilt): Xwayland
	 * makes them X input devices (tablet-v2) */
	struct wlr_tablet_manager_v2 *tablet_manager;
	struct wl_listener new_input;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	int cursor_size;              /* the xcursor_manager's size */
	bool xwayland_cursor;         /* Xwayland's own pointer was given that size */
	struct timespec cursor_checked; /* when the size was last looked at */
	struct wl_listener cursor_motion_relative;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	int32_t touch_id;
	double touch_lx;
	double touch_ly;
	struct wl_listener touch_down;
	struct wl_listener touch_up;
	struct wl_listener touch_motion;
	struct wl_listener touch_frame;

	struct wl_listener tablet_tool_axis;
	struct wl_listener tablet_tool_proximity;
	struct wl_listener tablet_tool_tip;
	struct wl_listener tablet_tool_button;

	struct wl_list drag_icons;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;

	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;

	/* sg-compositor: an elevated window being moved or resized by the user
	 * (its _NET_WM_MOVERESIZE), see elevated.c. */
	struct cg_view *grab_view;
	bool grab_resize;
	uint32_t grab_edges;
	double grab_cx, grab_cy;
	struct wlr_box grab_box;
	/* Alt+Tab pressed in an elevated window: switch on this key's release. */
	uint32_t switch_keycode;
};

struct cg_keyboard_group {
	struct wlr_keyboard_group *wlr_group;
	struct cg_seat *seat;
	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_list link; // cg_seat::keyboard_groups
	bool is_virtual;
	/* sg-compositor: a virtual keyboard's client (console shadow: its own
	 * keyboard is the remote side, every other one the console) */
	struct wl_client *owner;
};

struct cg_pointer {
	struct wl_list link; // seat::pointers
	struct cg_seat *seat;
	struct wlr_pointer *pointer;
	bool is_virtual; /* a privileged client's (remote access), not hardware */

	struct wl_listener destroy;
};

struct cg_touch {
	struct wl_list link; // seat::touch
	struct cg_seat *seat;
	struct wlr_touch *touch;

	struct wl_listener destroy;
};

struct cg_tablet {
	struct wl_list link; // seat::tablets
	struct cg_seat *seat;
	struct wlr_tablet *tablet;
	struct wlr_tablet_v2_tablet *tablet_v2;

	struct wl_listener destroy;
};

/* sg-compositor: a pen (tool) of a tablet, as programs see it */
struct cg_tablet_tool {
	struct cg_seat *seat;
	struct wlr_tablet_v2_tablet_tool *tool_v2;
	double tilt_x, tilt_y;
	/* its tip is down as the mouse's left button (over no program that
	 * takes a pen: a title bar the compositor draws, the desktop) */
	bool pointer_down;
	/* a stroke's surface and its offset from the layout */
	double grab_dx, grab_dy;

	struct wl_listener set_cursor;
	struct wl_listener destroy;
};

struct cg_drag_icon {
	struct wl_list link; // seat::drag_icons
	struct cg_seat *seat;
	struct wlr_drag_icon *wlr_drag_icon;
	struct wlr_scene_tree *scene_tree;

	/* The drag icon has a position in layout coordinates. */
	double lx, ly;

	struct wl_listener destroy;
};

struct cg_seat *seat_create(struct cg_server *server, struct wlr_backend *backend);
void seat_destroy(struct cg_seat *seat);
struct cg_view *seat_get_focus(struct cg_seat *seat);
void seat_set_focus(struct cg_seat *seat, struct cg_view *view);
void seat_center_cursor(struct cg_seat *seat);
/* Touch screens and built-in pens to the built-in screen (an output was
 * added or removed) */
void seat_map_builtin_inputs(struct cg_seat *seat);
/* The pointer's size: Settings' (effects.conf cursor=), else the screen's
 * recommended scale's share of 24 px; force: look now (an output changed),
 * else at most once a second. */
void seat_update_cursor_size(struct cg_seat *seat, bool force);
/* the recommended display scale for a WxH screen, in percent: the shorter
 * side over 1080 in 25% steps, 100% to 400% (sg-shell's scale_for_screen) */
int scale_for_screen(int w, int h);

#endif
