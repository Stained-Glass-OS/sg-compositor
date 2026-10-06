#ifndef CG_DECOR_H
#define CG_DECOR_H

#include <stdbool.h>
#include <stdint.h>

/* the title bar's height: the Wine frames' (Settings' effects.conf), else 32 */
int decor_title_h(void);

struct cg_seat;
struct cg_server;
struct cg_view;

/* A title bar for a Linux program's X11 window (decor.c); NULL if the
 * window should have none. */
void decor_create(struct cg_view *view);
void decor_destroy(struct cg_view *view);
/* the view has our title bar */
bool decor_has(struct cg_view *view);
/* the shell's taskbar's height: a maximized window stops above it -- as
 * Settings' effects.conf gives it (taskbar=: 40 px at 100%, 70 at 175%),
 * else 40 */
int decor_taskbar_h(void);
#define DECOR_TASKBAR_H decor_taskbar_h()
/* the pointer's size Settings' effects.conf gives (cursor=: 24 px at 100%,
 * the display scale's share), or 0 */
int decor_cursor_size(void);
void decor_set_active(struct cg_view *view, bool active);
void decor_set_fullscreen(struct cg_view *view, bool fullscreen);

/* Pointer input over title bars: true when it was theirs (not the window's). */
bool decor_button(struct cg_seat *seat, bool pressed, uint32_t time_msec);
bool decor_motion(struct cg_seat *seat, double lx, double ly);

#endif
