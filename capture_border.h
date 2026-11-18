#ifndef CG_CAPTURE_BORDER_H
#define CG_CAPTURE_BORDER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct cg_server;
struct wlr_output;
struct wlr_buffer;

/* sg-compositor: the capture border (capture_border.c) -- the thin yellow
 * frame Windows draws round a window or monitor that Windows.Graphics.Capture
 * is capturing, drawn here and never captured itself. */
void capture_border_init(struct cg_server *server);
void capture_border_fini(struct cg_server *server);
/* CAPTUREBORDER on the control socket: the connection is the border's */
void capture_border_take_connection(struct cg_server *server, int fd);
/* the session locked, a secure prompt, the security screen: no border */
void capture_border_update_visibility(struct cg_server *server);
/* does any border show on this part of the output (output buffer pixels)? */
bool capture_border_in(struct cg_server *server, struct wlr_output *output, const struct wlr_box *box);
/* the output rendered as it is shown, without the borders: a buffer the
 * caller unlocks (NULL: could not) */
struct wlr_buffer *capture_border_render_without(struct cg_server *server, struct wlr_output *output);

#endif
