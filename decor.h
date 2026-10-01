#ifndef CG_DECOR_H
#define CG_DECOR_H

#include <stdbool.h>
#include <stdint.h>

#define DECOR_TITLE_H 32

struct cg_seat;
struct cg_server;
struct cg_view;

/* A title bar for a Linux program's X11 window (decor.c); NULL if the
 * window should have none. */
void decor_create(struct cg_view *view);
void decor_destroy(struct cg_view *view);
/* the view has our title bar */
bool decor_has(struct cg_view *view);
#define DECOR_TASKBAR_H 40 /* the shell's taskbar: a maximized window stops above it */
void decor_set_active(struct cg_view *view, bool active);
void decor_set_fullscreen(struct cg_view *view, bool fullscreen);

/* Pointer input over title bars: true when it was theirs (not the window's). */
bool decor_button(struct cg_seat *seat, bool pressed, uint32_t time_msec);
bool decor_motion(struct cg_seat *seat, double lx, double ly);

#endif
