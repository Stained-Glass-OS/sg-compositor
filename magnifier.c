/*
 * sg-compositor: Magnifier, drawn by the compositor.
 *
 * Windows' Magnifier magnifies everything on the screen because the desktop
 * window manager does the scaling. Ours did it in magnify.exe, from what Wine
 * could paint of its own windows, so X and Wayland programs were missing or
 * black. Now the compositor draws each output's frame (the whole scene, every
 * kind of window) into a buffer of its own and puts it on the screen scaled:
 *
 *   full  the whole output at zoom z, its source origin p (1 - 1/z) for the
 *         pointer p -- the point under the pointer stays under it, so input
 *         needs no transform -- or an origin the controller gives
 *         (MagSetFullscreenTransform);
 *   lens  the screen as it is, and a rectangle round the pointer showing what
 *         is round it at zoom z;
 *   dock  the screen as it is, and the docked view's rectangle showing what is
 *         round the pointer (or a centre given: focus, caret).
 *
 * Colours inverted on request. The pointer is drawn into the frame (software
 * cursor) while the whole output is magnified, so it is magnified too.
 *
 * Nothing is magnified while a lock, a secure prompt or the security screen
 * is up; it comes back after. No pixels go to any client: the controller only
 * says what to show. The controller is the MAGNIFY connection on the control
 * socket (anyone in the session, like INHIBIT); when it closes, magnification
 * ends -- a Magnifier that crashed leaves no magnified screen behind.
 *
 * Its lines, each answered "OK" or "ERR":
 *   off | full Z [X Y] | lens Z W H | dock Z X Y W H | centre pointer |
 *   centre X Y | invert 0|1 | smooth 0|1
 * Z is percent. Coordinates are the Wine desktop's -- its top left is 0,0,
 * as for Windows programs the virtual screen's top left is (the controller
 * takes that off) -- so they hold wherever the desktop sits in the layout
 * (displays left of or above the first, a layout not at 0,0); with no Wine
 * desktop they are the layout's. An X pixel is a layout pixel at any scale.
 */

#define _POSIX_C_SOURCE 200809L

#include "magnifier.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <drm_fourcc.h>

#include "output.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

/* where the controller's coordinates start in the layout: the Wine desktop's top left */
static void
given_origin(struct cg_server *server, double *x, double *y)
{
	*x = *y = 0;
#if CAGE_HAS_XWAYLAND
	struct cg_view *view;
	wl_list_for_each (view, &server->views, link) {
		if (view->type == CAGE_XWAYLAND_VIEW && xwayland_view_is_shell_desktop(view)) {
#ifndef SG_MUTANT_MAGNIFY_LAYOUT_COORDS
			*x = view->lx;
			*y = view->ly;
#endif
			return;
		}
	}
#endif
}

static void
schedule_all(struct cg_server *server, bool whole)
{
	struct cg_output *output;
	wl_list_for_each (output, &server->outputs, link) {
		if (whole && output->scene_output) {
			wlr_damage_ring_add_whole(&output->scene_output->damage_ring);
		}
		wlr_output_schedule_frame(output->wlr_output);
	}
}

bool
magnifier_active(struct cg_server *server)
{
	const struct cg_magnifier *m = &server->magnifier;
	if (m->mode == CG_MAGNIFIER_OFF) {
		return false;
	}
#ifndef SG_MUTANT_MAGNIFY_WHEN_SECURE
	if (server->lock.locked || server->lock.secure || server->lock.sas) {
		return false;
	}
#endif
	return true;
}

void
magnifier_pointer_moved(struct cg_server *server)
{
	if (magnifier_active(server)) {
		schedule_all(server, false);
	}
}

void
magnifier_state_changed(struct cg_server *server)
{
	if (server->magnifier.mode != CG_MAGNIFIER_OFF) {
		schedule_all(server, true);
	}
}

/* ---- the controller ------------------------------------------------------ */

static void
set_off(struct cg_server *server)
{
	struct cg_magnifier *m = &server->magnifier;
	bool was = m->mode != CG_MAGNIFIER_OFF;
	m->mode = CG_MAGNIFIER_OFF;
	m->origin_given = m->centre_given = false;
	m->invert = false;
	if (was) {
		schedule_all(server, true);
	}
}

static bool
command(struct cg_server *server, const char *line)
{
	struct cg_magnifier *m = &server->magnifier;
	int z, a, b, c, d, n = 0;
	char word[16];

	if (sscanf(line, "%15s%n", word, &n) != 1) {
		return false;
	}
	line += n;

	if (!strcmp(word, "off")) {
		set_off(server);
		return true;
	} else if (!strcmp(word, "full")) {
		int k = sscanf(line, "%d %d %d", &z, &a, &b);
		if (k != 1 && k != 3) {
			return false;
		}
		m->mode = CG_MAGNIFIER_FULL;
		m->origin_given = k == 3;
		m->origin_x = k == 3 ? a : 0;
		m->origin_y = k == 3 ? b : 0;
	} else if (!strcmp(word, "lens")) {
		if (sscanf(line, "%d %d %d", &z, &a, &b) != 3 || a < 16 || b < 16 || a > 16384 || b > 16384) {
			return false;
		}
		m->mode = CG_MAGNIFIER_LENS;
		m->lens_w = a;
		m->lens_h = b;
	} else if (!strcmp(word, "dock")) {
		if (sscanf(line, "%d %d %d %d %d", &z, &a, &b, &c, &d) != 5 || c < 1 || d < 1 || c > 16384 || d > 16384) {
			return false;
		}
		m->mode = CG_MAGNIFIER_DOCK;
		m->dock = (struct wlr_box){a, b, c, d};
	} else if (!strcmp(word, "centre") || !strcmp(word, "center")) {
		if (sscanf(line, "%d %d", &a, &b) == 2) {
			m->centre_given = true;
			m->centre_x = a;
			m->centre_y = b;
		} else if (sscanf(line, "%15s", word) == 1 && !strcmp(word, "pointer")) {
			m->centre_given = false;
		} else {
			return false;
		}
		schedule_all(server, false);
		return true;
	} else if (!strcmp(word, "invert") || !strcmp(word, "smooth")) {
		if (sscanf(line, "%d", &a) != 1) {
			return false;
		}
		if (word[0] == 'i') {
			m->invert = a != 0;
		} else {
			m->smooth = a != 0;
		}
		schedule_all(server, true);
		return true;
	} else {
		return false;
	}

	m->zoom = z < 100 ? 100 : z > 1600 ? 1600 : z;
	schedule_all(server, true);
	return true;
}

static void
drop_connection(struct cg_server *server)
{
	struct cg_magnifier *m = &server->magnifier;
	if (m->source) {
		wl_event_source_remove(m->source);
		m->source = NULL;
	}
	if (m->fd >= 0) {
		close(m->fd);
		m->fd = -1;
	}
	m->len = 0;
	set_off(server);
}

static int
handle_readable(int fd, uint32_t mask, void *data)
{
	struct cg_server *server = data;
	struct cg_magnifier *m = &server->magnifier;
	ssize_t n;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		drop_connection(server);
		return 0;
	}
	n = recv(fd, m->line + m->len, sizeof(m->line) - 1 - m->len, 0);
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
		return 0;
	}
	if (n <= 0) {
		drop_connection(server);
		return 0;
	}
	m->len += n;
	m->line[m->len] = 0;

	char *nl;
	while ((nl = memchr(m->line, '\n', m->len))) {
		*nl = 0;
		const char *reply = command(server, m->line) ? "OK\n" : "ERR\n";
		size_t used = nl + 1 - m->line;
		memmove(m->line, nl + 1, m->len - used);
		m->len -= used;
		m->line[m->len] = 0;
		if (send(fd, reply, strlen(reply), MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
			drop_connection(server);
			return 0;
		}
	}
	if (m->len == sizeof(m->line) - 1) {
		drop_connection(server); /* a line that never ends */
	}
	return 0;
}

void
magnifier_take_connection(struct cg_server *server, int fd)
{
	struct cg_magnifier *m = &server->magnifier;

	/* one controller: a new Magnifier takes over from an old one */
	if (m->fd >= 0) {
		drop_connection(server);
	}
	m->fd = fd;
	m->len = 0;
	m->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), fd, WL_EVENT_READABLE,
					 handle_readable, server);
	if (send(fd, "OK magnify\n", 11, MSG_NOSIGNAL) < 0) {
		drop_connection(server);
	}
}

void
magnifier_init(struct cg_server *server)
{
	struct cg_magnifier *m = &server->magnifier;
	memset(m, 0, sizeof(*m));
	m->fd = -1;
	m->zoom = 200;
	m->smooth = true;
	m->lens_w = 400;
	m->lens_h = 250;
}

void
magnifier_fini(struct cg_server *server)
{
	struct cg_magnifier *m = &server->magnifier;
	if (m->source) {
		wl_event_source_remove(m->source);
		m->source = NULL;
	}
	if (m->fd >= 0) {
		close(m->fd);
		m->fd = -1;
	}
}

/* ---- drawing --------------------------------------------------------------- */

static double
clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Inverting a rectangle of the finished frame: in its memory when the CPU
 * can reach it (pixman, shared memory), else with GL. */
static GLuint invert_program;
static EGLContext invert_program_ctx;

static bool
invert_cpu(struct wlr_buffer *buffer, const struct wlr_box *r)
{
	void *data;
	uint32_t format;
	size_t stride;

	if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ | WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
					      &data, &format, &stride)) {
		return false;
	}
	bool ok = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XBGR8888 ||
		  format == DRM_FORMAT_ABGR8888;
	if (ok) {
		for (int y = r->y; y < r->y + r->height; y++) {
			uint32_t *row = (uint32_t *) ((uint8_t *) data + (size_t) y * stride);
			for (int x = r->x; x < r->x + r->width; x++) {
				row[x] ^= 0x00ffffffu;
			}
		}
	}
	wlr_buffer_end_data_ptr_access(buffer);
	return ok;
}

static GLuint
compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static bool
invert_gl(struct wlr_renderer *renderer, struct wlr_buffer *buffer, const struct wlr_box *r)
{
	if (!wlr_renderer_is_gles2(renderer)) {
		return false;
	}
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
	EGLDisplay dpy = wlr_egl_get_display(egl);
	EGLContext ctx = wlr_egl_get_context(egl);
	EGLDisplay prev_dpy = eglGetCurrentDisplay();
	EGLContext prev_ctx = eglGetCurrentContext();
	EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW), prev_read = eglGetCurrentSurface(EGL_READ);

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		return false;
	}
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(renderer, buffer);
	bool ok = fbo != 0;

	if (ok && (!invert_program || invert_program_ctx != ctx)) {
		GLuint vs = compile(GL_VERTEX_SHADER, "attribute vec2 pos;\n"
						      "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n");
		GLuint fs = compile(GL_FRAGMENT_SHADER, "precision mediump float;\n"
							"void main() { gl_FragColor = vec4(1.0); }\n");
		GLint linked = 0;
		invert_program = vs && fs ? glCreateProgram() : 0;
		if (invert_program) {
			glAttachShader(invert_program, vs);
			glAttachShader(invert_program, fs);
			glBindAttribLocation(invert_program, 0, "pos");
			glLinkProgram(invert_program);
			glGetProgramiv(invert_program, GL_LINK_STATUS, &linked);
			if (!linked) {
				glDeleteProgram(invert_program);
				invert_program = 0;
			}
		}
		if (vs) {
			glDeleteShader(vs);
		}
		if (fs) {
			glDeleteShader(fs);
		}
		invert_program_ctx = ctx;
		ok = invert_program != 0;
	}

	if (ok) {
		static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, buffer->width, buffer->height);
		/* wlroots' frames are top row first in their framebuffers */
		glEnable(GL_SCISSOR_TEST);
		glScissor(r->x, r->y, r->width, r->height);
		glEnable(GL_BLEND);
		/* white, blended as 1 - destination: the colours inverted, alpha kept */
		glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
		glUseProgram(invert_program);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
		glEnableVertexAttribArray(0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray(0);
		glUseProgram(0);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_BLEND);
		glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		/* done before the frame is shown, whatever sync the commit uses */
		glFinish();
	}

	if (prev_ctx != EGL_NO_CONTEXT) {
		eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx);
	} else {
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}
	return ok;
}

static void
invert_rect(struct wlr_renderer *renderer, struct wlr_buffer *buffer, const struct wlr_box *r)
{
	static bool warned;
	if (r->width <= 0 || r->height <= 0) {
		return;
	}
	if (!invert_cpu(buffer, r) && !invert_gl(renderer, buffer, r) && !warned) {
		warned = true;
		wlr_log(WLR_ERROR, "magnifier: this renderer cannot invert colours");
	}
}

static void
draw_scaled(struct wlr_render_pass *pass, struct wlr_texture *tex, double sx, double sy, double sw, double sh,
	    const struct wlr_box *dst, bool smooth)
{
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
						  .texture = tex,
						  .src_box = {sx, sy, sw, sh},
						  .dst_box = *dst,
						  .filter_mode = smooth ? WLR_SCALE_FILTER_BILINEAR : WLR_SCALE_FILTER_NEAREST,
						  .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
					  });
}

static void
draw_border(struct wlr_render_pass *pass, const struct wlr_box *b, int t)
{
	const struct wlr_render_color c = {0.0f, 0.47f, 0.84f, 1.0f}; /* the accent's blue */
	struct wlr_box edges[4] = {
		{b->x - t, b->y - t, b->width + 2 * t, t},
		{b->x - t, b->y + b->height, b->width + 2 * t, t},
		{b->x - t, b->y, t, b->height},
		{b->x + b->width, b->y, t, b->height},
	};
	for (int i = 0; i < 4; i++) {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){.box = edges[i], .color = c});
	}
}

static void
cursor_lock(struct cg_output *output, bool want)
{
	if (output->mag_cursor_locked != want) {
		wlr_output_lock_software_cursors(output->wlr_output, want);
		output->mag_cursor_locked = want;
	}
}

void
magnifier_output_destroy(struct cg_output *output)
{
	cursor_lock(output, false);
	if (output->mag_swapchain) {
		wlr_swapchain_destroy(output->mag_swapchain);
		output->mag_swapchain = NULL;
	}
}

bool
magnifier_render_output(struct cg_output *output)
{
	struct cg_server *server = output->server;
	const struct cg_magnifier *m = &server->magnifier;
	struct wlr_output *wo = output->wlr_output;

	if (!magnifier_active(server) || !output->scene_output) {
		if (output->mag_was_active) {
			/* the frames it drew are not the scene's: all of it again */
			output->mag_was_active = false;
			cursor_lock(output, false);
			wlr_damage_ring_add_whole(&output->scene_output->damage_ring);
		}
		return false;
	}

	struct wlr_box ob;
	wlr_output_layout_get_box(server->output_layout, wo, &ob);
	if (wlr_box_empty(&ob) || wo->width <= 0 || wo->height <= 0) {
		return false;
	}

	/* the whole output under a magnified pointer: drawn into the frame, scaled with it */
	cursor_lock(output, m->mode == CG_MAGNIFIER_FULL || m->mode == CG_MAGNIFIER_LENS);
	output->mag_was_active = true;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_output_configure_primary_swapchain(wo, &state, &wo->swapchain)) {
		wlr_output_state_finish(&state);
		return false;
	}
	if (output->mag_swapchain &&
	    (output->mag_swapchain->width != wo->swapchain->width ||
	     output->mag_swapchain->height != wo->swapchain->height)) {
		wlr_swapchain_destroy(output->mag_swapchain);
		output->mag_swapchain = NULL;
	}
	if (!output->mag_swapchain) {
		output->mag_swapchain = wlr_swapchain_create(server->allocator, wo->swapchain->width,
							     wo->swapchain->height, &wo->swapchain->format);
		if (!output->mag_swapchain) {
			wlr_output_state_finish(&state);
			return false;
		}
	}

	/* the scene, all of it, into a buffer of our own */
	wlr_damage_ring_add_whole(&output->scene_output->damage_ring);
	struct wlr_scene_output_state_options opts = {.swapchain = output->mag_swapchain};
	if (!wlr_scene_output_build_state(output->scene_output, &state, &opts) ||
	    !(state.committed & WLR_OUTPUT_STATE_BUFFER) || !state.buffer) {
		wlr_output_state_finish(&state);
		return false;
	}

	struct wlr_buffer *scene_buf = wlr_buffer_lock(state.buffer);
	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, scene_buf);
	struct wlr_buffer *dst = tex ? wlr_swapchain_acquire(wo->swapchain, NULL) : NULL;
	struct wlr_render_pass *pass = dst ? wlr_renderer_begin_buffer_pass(server->renderer, dst, NULL) : NULL;
	if (!pass) {
		if (dst) {
			wlr_buffer_unlock(dst);
		}
		if (tex) {
			wlr_texture_destroy(tex);
		}
		wlr_buffer_unlock(scene_buf);
		wlr_output_state_finish(&state);
		return false;
	}

	double W = dst->width, H = dst->height, z = m->zoom / 100.0;
	double scale = wo->scale > 0 ? wo->scale : 1.0;
	struct wlr_cursor *cursor = server->seat->cursor;
	double px = clampd((cursor->x - ob.x) * scale, 0, W), py = clampd((cursor->y - ob.y) * scale, 0, H);
	/* the output's top left in the controller's coordinates */
	double dx, dy;
	given_origin(server, &dx, &dy);
	double gx = ob.x - dx, gy = ob.y - dy;
	struct wlr_box whole = {0, 0, (int) W, (int) H}, inverted = whole;

	if (m->mode == CG_MAGNIFIER_FULL) {
		double sw = W / z, sh = H / z, sx, sy;
		if (m->origin_given) {
			sx = (m->origin_x - gx) * scale;
			sy = (m->origin_y - gy) * scale;
		} else {
			/* the point under the pointer stays under the pointer */
			sx = px - px / z;
			sy = py - py / z;
#ifdef SG_MUTANT_MAGNIFY_NO_FOLLOW
			sx = sy = 0;
#endif
		}
		sx = clampd(sx, 0, W - sw);
		sy = clampd(sy, 0, H - sh);
		draw_scaled(pass, tex, sx, sy, sw, sh, &whole, m->smooth);
	} else {
		draw_scaled(pass, tex, 0, 0, W, H, &whole, false);

		struct wlr_box view;
		double cx = px, cy = py;
		if (m->mode == CG_MAGNIFIER_LENS) {
			int lw = (int) fmin(m->lens_w * scale, W), lh = (int) fmin(m->lens_h * scale, H);
			view = (struct wlr_box){(int) clampd(px - lw / 2.0, 0, W - lw), (int) clampd(py - lh / 2.0, 0, H - lh),
						lw, lh};
		} else {
			struct wlr_box dock = {(int) lround((m->dock.x - gx) * scale), (int) lround((m->dock.y - gy) * scale),
					       (int) lround(m->dock.width * scale), (int) lround(m->dock.height * scale)};
			if (!wlr_box_intersection(&view, &dock, &whole)) {
				view = (struct wlr_box){0};
			}
			if (m->centre_given) {
				cx = clampd((m->centre_x - gx) * scale, 0, W);
				cy = clampd((m->centre_y - gy) * scale, 0, H);
			}
		}
		if (!wlr_box_empty(&view)) {
			double sw = view.width / z, sh = view.height / z;
			double sx = clampd(cx - sw / 2, 0, W - sw), sy = clampd(cy - sh / 2, 0, H - sh);
			draw_scaled(pass, tex, sx, sy, sw, sh, &view, m->smooth);
			if (m->mode == CG_MAGNIFIER_LENS) {
				draw_border(pass, &view, (int) fmax(1, lround(2 * scale)));
			}
		}
		inverted = view;
	}

	bool ok = wlr_render_pass_submit(pass);
#ifdef SG_MUTANT_MAGNIFY_NO_INVERT
	ok = ok || inverted.width < 0; /* the colours left as they are */
	if (0)
#endif
	if (ok && m->invert) {
		invert_rect(server->renderer, dst, &inverted);
	}

	wlr_texture_destroy(tex);
	wlr_buffer_unlock(scene_buf);
	if (ok) {
		wlr_output_state_set_buffer(&state, dst);
		/* the whole frame is new */
		state.committed &= ~WLR_OUTPUT_STATE_DAMAGE;
		ok = wlr_output_commit_state(wo, &state);
	}
	wlr_buffer_unlock(dst);
	wlr_output_state_finish(&state);
	return ok;
}
