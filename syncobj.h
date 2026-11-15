#ifndef CG_SYNCOBJ_H
#define CG_SYNCOBJ_H

#include <wayland-server-core.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>

/* linux-drm-syncobj-v1 on wlroots 0.18 (syncobj.c): with NVIDIA's driver */
void cg_syncobj_init(struct wl_display *display, struct wlr_renderer *renderer, struct wlr_compositor *compositor);
/* an output's frame: release points whose buffers were let go are signalled */
void cg_syncobj_frame(void);

#endif
