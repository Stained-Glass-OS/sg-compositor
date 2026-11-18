#ifndef CG_OUTPUT_H
#define CG_OUTPUT_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>

#include "server.h"
#include "view.h"

struct cg_output {
	struct cg_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;

	struct wl_listener commit;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_listener frame;

	/* sg-compositor: turned off by a display-power request (wlopm, swayidle),
	 * not by configuration -- the next input turns it back on. */
	bool powered_off;

	/* sg-compositor: a few seconds after the output comes on (woken, or a
	 * monitor that returned), its mode is set once more (output_relink) */
	struct wl_event_source *relink_timer;

	/* sg-compositor: Magnifier (magnifier.c): the scene is drawn into these
	 * and put on the screen scaled */
	struct wlr_swapchain *mag_swapchain;
	bool mag_was_active;
	bool mag_cursor_locked;

	struct wl_list link; // cg_server::outputs
};

void handle_output_manager_apply(struct wl_listener *listener, void *data);
void handle_output_manager_test(struct wl_listener *listener, void *data);
void handle_output_layout_change(struct wl_listener *listener, void *data);
void handle_new_output(struct wl_listener *listener, void *data);
void handle_output_power_set_mode(struct wl_listener *listener, void *data);
void output_power_wake(struct cg_server *server);
void output_set_window_title(struct cg_output *output, const char *title);

#endif
