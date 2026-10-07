#ifndef CG_WAYLAND_APP_H
#define CG_WAYLAND_APP_H

#include <stdbool.h>
#include <sys/types.h>

struct cg_seat;
struct cg_view;
struct cg_xdg_shell_view;

/* sg-compositor: a user program's native Wayland window managed like the
 * session's other windows (wayland_app.c) */
void wayland_app_init(struct cg_xdg_shell_view *xv);
void wayland_app_fini(struct cg_xdg_shell_view *xv);
bool wayland_app_is(struct cg_view *view);
/* on the taskbar: a program's main window, not a dialog */
bool wayland_app_listed(struct cg_view *view);
unsigned long wayland_app_window_id(struct cg_view *view);
const char *wayland_app_class(struct cg_view *view);

void wayland_app_initial_configure(struct cg_view *view);
void wayland_app_place(struct cg_view *view);
void wayland_app_commit(struct cg_view *view);
void wayland_app_request_fullscreen(struct cg_view *view);

void wayland_app_activate(struct cg_view *view);
void wayland_app_minimize(struct cg_view *view);
void wayland_app_close(struct cg_view *view);
bool wayland_app_kill(struct cg_view *view, uid_t uid);

/* the user moving or resizing one (pointer motion; a button released) */
bool wayland_app_grab_motion(struct cg_seat *seat, double lx, double ly);
void wayland_app_grab_end(struct cg_seat *seat);

#endif
