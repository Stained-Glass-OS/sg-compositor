#ifndef CG_IDLE_INHIBIT_H
#define CG_IDLE_INHIBIT_H

#include <wayland-server-core.h>

struct cg_server;

void handle_idle_inhibitor_v1_new(struct wl_listener *listener, void *data);

/* Inhibit idle while any Wayland inhibitor or control-socket INHIBIT is held. */
void idle_inhibit_v1_check_active(struct cg_server *server);

#endif
