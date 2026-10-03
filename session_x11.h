#ifndef CG_SESSION_X11_H
#define CG_SESSION_X11_H

#include <stdbool.h>
#include <stddef.h>

struct cg_server;

/* sg-compositor: the session's own X11 programs' windows (session_x11.c) */
size_t session_x11_list(struct cg_server *server, char *buf, size_t len);
bool session_x11_activate(struct cg_server *server, unsigned long window);
bool session_x11_minimize(struct cg_server *server, unsigned long window);
bool session_x11_close(struct cg_server *server, unsigned long window);
bool session_x11_desktop_front(struct cg_server *server);
struct cg_view;
bool session_x11_super(struct cg_server *server, struct cg_view *focus);
/* a Linux program's window, mapped while the taskbar frames them: not shown
 * until framed (unhold, as it unmaps) or a moment has passed */
void session_x11_hold(struct cg_view *view);
void session_x11_unhold(struct cg_view *view);
bool session_x11_held(struct cg_view *view);

#endif
