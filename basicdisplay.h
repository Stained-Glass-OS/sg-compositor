#ifndef CG_BASICDISPLAY_H
#define CG_BASICDISPLAY_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct cg_server;

/* sg-compositor: the Basic Display Adapter (basicdisplay.c). When no GPU
 * driver gives the machine a DRM device, the session draws in software and
 * shows on the firmware's framebuffer (/dev/fb0: efifb, vesafb). */

/* Called when wlr_backend_autocreate failed: a session, libinput and a
 * headless backend whose one output is copied to the framebuffer. Sets
 * server->backend and server->session; false if there is no framebuffer
 * either. */
bool basic_display_create(struct cg_server *server, struct wl_event_loop *loop);

/* After wlr_backend_start: add the output (sized like the framebuffer). */
void basic_display_start(struct cg_server *server);

bool basic_display_active(void);

#endif
