#ifndef CG_MAGNIFIER_H
#define CG_MAGNIFIER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct cg_server;
struct cg_output;

/* sg-compositor: Magnifier (magnifier.c). The screen is magnified where it is
 * drawn, as Windows' desktop window manager does it for its Magnifier: every
 * window -- Wine's, X and Wayland programs', elevated programs' -- and no
 * pixels handed to anyone. */
enum cg_magnifier_mode {
	CG_MAGNIFIER_OFF,
	CG_MAGNIFIER_FULL, /* the whole display, around the pointer (or an origin given) */
	CG_MAGNIFIER_LENS, /* a rectangle round the pointer */
	CG_MAGNIFIER_DOCK, /* a band (the docked view's rectangle) showing what is round a centre */
};

struct cg_magnifier {
	enum cg_magnifier_mode mode;
	int zoom; /* percent, 100..1600 */
	bool invert;
	bool smooth;
	int lens_w, lens_h;     /* layout pixels */
	struct wlr_box dock;    /* layout coordinates */
	bool origin_given;      /* full: the source's top left, as MagSetFullscreenTransform gives it */
	double origin_x, origin_y;
	bool centre_given;      /* dock: what it shows is round this point, not the pointer */
	double centre_x, centre_y;

	/* MAGNIFY connections on the control socket (struct cg_mag_conn): the
	 * screen's view (full, lens, dock) is the last one's to set it and lasts
	 * as long as it does; window views (magnifier controls, MagSetWindowSource)
	 * belong to the connection that made them and go with it */
	struct wl_list conns;
	int n_conns;
	struct cg_mag_conn *owner;
	struct wl_list windows; /* struct cg_mag_window */
};

#define CG_MAG_WINDOW_CLIPS 16

/* a magnifier control's view: what of the screen (src, the Wine desktop's
 * coordinates) is shown in its client area (dst), seen through the parts of
 * it no other window covers (clip) */
struct cg_mag_window {
	struct cg_mag_conn *conn;
	unsigned int id;
	double sx, sy, sw, sh;
	struct wlr_box dst;
	bool invert;
	int n_clip;
	struct wlr_box clip[CG_MAG_WINDOW_CLIPS];
	struct wl_list link;
};

void magnifier_init(struct cg_server *server);
void magnifier_fini(struct cg_server *server);

/* MAGNIFY on the control socket: this connection controls it from now on */
void magnifier_take_connection(struct cg_server *server, int fd);

/* whether it draws now: on, and nothing secure (lock, consent prompt,
 * security screen) is up -- those are never magnified */
bool magnifier_active(struct cg_server *server);

/* the output's frame, magnified: false if it was not (draw it as usual) */
bool magnifier_render_output(struct cg_output *output);

/* the output is going away */
void magnifier_output_destroy(struct cg_output *output);

/* the pointer moved / the lock state changed: frames again */
void magnifier_pointer_moved(struct cg_server *server);
void magnifier_state_changed(struct cg_server *server);

#endif
