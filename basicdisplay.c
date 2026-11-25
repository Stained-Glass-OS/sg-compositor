/*
 * sg-compositor: the Basic Display Adapter.
 *
 * Windows always reaches its sign-in screen and a desktop: with no driver for
 * the graphics card it draws through the Basic Display Adapter, in software,
 * on the framebuffer the firmware set up. Here the same: when no GPU driver
 * gives the machine a DRM device (nomodeset, a card whose driver is not in
 * the kernel or failed to load -- NVIDIA without its firmware or driver, a
 * GPU newer than the kernel), wlroots finds no GPU and cannot start, and the
 * login screen was a black screen. Debian's kernel has no simpledrm (the DRM
 * driver for the firmware's framebuffer), only efifb/vesafb: fbdev, which
 * wlroots cannot drive.
 *
 * So this compositor then runs on a headless backend with the pixman
 * (software) renderer, input from libinput through the seat as usual, and
 * copies each frame's damage into a shared frame (a sealed memfd) that
 * sg-basic-displayd, root, puts on /dev/fb0 -- only while this session is
 * the active one on seat0. The compositor never opens the framebuffer:
 * logind does not hand it out (TakeDevice is for DRM and input), and an open
 * framebuffer would stay readable after a switch to another user's session.
 * VT switching works as with a GPU (the session's active signal).
 *
 * SG_BASIC_DISPLAY=1 uses it even when a GPU is there (tests);
 * SG_BASIC_DISPLAY=0 never.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "basicdisplay.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "basicdisplay_proto.h"
#include "server.h"

static struct {
	bool on;
	struct wlr_session *session;
	struct wlr_backend *headless;
	int sock;
	uint8_t *frame; /* shared with sg-basic-displayd: XRGB8888, rows of width*4 */
	uint32_t width, height;
	struct wlr_output *output;
	struct wlr_buffer *last; /* kept: drawn again when the console comes back */
	bool need_full;          /* a frame was missed: the next one is drawn whole */
	time_t last_try;         /* the last attempt to reach sg-basic-displayd again */
	struct wl_listener commit;
	struct wl_listener output_destroy;
	struct wl_listener active;
} bd = {.sock = -1};

bool
basic_display_active(void)
{
	return bd.on;
}

static void
send_damage(const struct sg_bd_rect *rects, int n)
{
	while (n > 0) {
		struct sg_bd_msg msg = {.type = SG_BD_DAMAGE};
		msg.count = n > SG_BD_MAX_RECTS ? SG_BD_MAX_RECTS : n;
		memcpy(msg.rects, rects, msg.count * sizeof(*rects));
		if (send(bd.sock, &msg, 8 + msg.count * sizeof(*rects), MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
			errno != EAGAIN) {
			/* restarted (an upgrade): connect again at the next frame */
			wlr_log_errno(WLR_ERROR, "basic display: sg-basic-displayd went away");
			close(bd.sock);
			bd.sock = -1;
			bd.need_full = true;
			return;
		}
		rects += msg.count;
		n -= msg.count;
	}
}

/* Copy a box of the frame into the shared frame; clipped. */
static bool
copy_box(const uint8_t *src, size_t src_stride, struct sg_bd_rect *r)
{
	if (r->x < 0) {
		r->w += r->x;
		r->x = 0;
	}
	if (r->y < 0) {
		r->h += r->y;
		r->y = 0;
	}
	if (r->x + r->w > (int)bd.width) {
		r->w = bd.width - r->x;
	}
	if (r->y + r->h > (int)bd.height) {
		r->h = bd.height - r->y;
	}
	if (r->w <= 0 || r->h <= 0) {
		return false;
	}
	for (int row = r->y; row < r->y + r->h; row++) {
		memcpy(bd.frame + ((size_t)row * bd.width + r->x) * 4, src + row * src_stride + r->x * 4,
		       (size_t)r->w * 4);
	}
	return true;
}

static bool connect_daemon(void);

/* Show the damaged part (NULL: all) of a frame. */
static void
show(struct wlr_buffer *buffer, const pixman_region32_t *damage)
{
	if (bd.sock < 0 && bd.session->active) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec != bd.last_try) { /* at most once a second */
			bd.last_try = now.tv_sec;
			if (connect_daemon()) {
				bd.need_full = true;
			} else if (bd.sock >= 0) {
				close(bd.sock);
				bd.sock = -1;
			}
		}
	}
	if (!bd.session->active || !bd.frame || bd.sock < 0) {
		bd.need_full = true;
		return;
	}
	if (bd.need_full) {
		damage = NULL;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		wlr_log(WLR_ERROR, "basic display: the frame cannot be read");
		bd.need_full = true;
		return;
	}
	struct sg_bd_rect rects[256];
	int n = 0;
	if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) {
		wlr_log(WLR_ERROR, "basic display: frames in format 0x%08x cannot be shown", format);
	} else {
		int count = 1;
		pixman_box32_t whole = {0, 0, buffer->width, buffer->height};
		const pixman_box32_t *b = &whole;
		if (damage) {
			b = pixman_region32_rectangles((pixman_region32_t *)damage, &count);
		}
		if (count > 256) { /* many small boxes: their bounds */
			whole = *pixman_region32_extents((pixman_region32_t *)damage);
			b = &whole;
			count = 1;
		}
		for (int i = 0; i < count; i++) {
			struct sg_bd_rect r = {b[i].x1, b[i].y1, b[i].x2 - b[i].x1, b[i].y2 - b[i].y1};
			if (copy_box(data, stride, &r)) {
				rects[n++] = r;
			}
		}
	}
	wlr_buffer_end_data_ptr_access(buffer);
	bd.need_full = false;
	send_damage(rects, n);
}

static void
handle_commit(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_commit *event = data;
	const struct wlr_output_state *state = event->state;
	if (!(state->committed & WLR_OUTPUT_STATE_BUFFER) || !state->buffer) {
		return;
	}
	/* damage is what changed since the frame before, which the shared
	 * frame already holds */
	bool partial = (state->committed & WLR_OUTPUT_STATE_DAMAGE) && bd.last;
	if (bd.last) {
		wlr_buffer_unlock(bd.last);
	}
	bd.last = wlr_buffer_lock(state->buffer);
	show(bd.last, partial ? &state->damage : NULL);
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	wl_list_remove(&bd.commit.link);
	wl_list_remove(&bd.output_destroy.link);
	if (bd.last) {
		wlr_buffer_unlock(bd.last);
		bd.last = NULL;
	}
	bd.output = NULL;
}

static void
handle_active(struct wl_listener *listener, void *data)
{
	if (bd.session->active) {
		wlr_log(WLR_INFO, "basic display: the console is back; drawing the whole frame");
		if (bd.last) {
			show(bd.last, NULL);
		}
		if (bd.output) {
			wlr_output_schedule_frame(bd.output);
		}
	}
}

/* Connect to sg-basic-displayd and share a frame with it. */
static bool
connect_daemon(void)
{
	const char *path = getenv("SG_BD_SOCKET") ? getenv("SG_BD_SOCKET") : SG_BD_SOCKET;
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	bd.sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (bd.sock < 0 || connect(bd.sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		wlr_log_errno(WLR_ERROR, "basic display: cannot reach sg-basic-displayd at %s", path);
		return false;
	}
	struct timeval tv = {5, 0};
	setsockopt(bd.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	struct sg_bd_hello hello;
	if (recv(bd.sock, &hello, sizeof(hello), 0) != sizeof(hello) || hello.magic != SG_BD_MAGIC ||
		hello.width < 320 || hello.height < 200 || hello.width > 16384 || hello.height > 16384) {
		wlr_log(WLR_ERROR, "basic display: sg-basic-displayd has no framebuffer for this session");
		return false;
	}
	hello.id[sizeof(hello.id) - 1] = 0;
	if (bd.frame) { /* connecting again */
		munmap(bd.frame, (size_t)bd.width * bd.height * 4);
		bd.frame = NULL;
	}
	bd.width = hello.width;
	bd.height = hello.height;
	size_t len = (size_t)bd.width * bd.height * 4;
	int mfd = memfd_create("sg-basic-display", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (mfd < 0 || ftruncate(mfd, len) < 0 || fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
		wlr_log_errno(WLR_ERROR, "basic display: no shared frame");
		return false;
	}
	bd.frame = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	if (bd.frame == MAP_FAILED) {
		bd.frame = NULL;
		close(mfd);
		return false;
	}
	struct sg_bd_msg msg = {.type = SG_BD_FRAME};
	char cbuf[CMSG_SPACE(sizeof(int))] = {0};
	struct iovec iov = {&msg, 8};
	struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &mfd, sizeof(int));
	bool ok = sendmsg(bd.sock, &mh, MSG_NOSIGNAL) == 8;
	close(mfd);
	if (ok) {
		wlr_log(WLR_INFO, "basic display: drawing in software on the firmware framebuffer (%s, %ux%u, %u bpp)",
			hello.id, bd.width, bd.height, hello.bpp);
	}
	return ok;
}

bool
basic_display_create(struct cg_server *server, struct wl_event_loop *loop)
{
	if (!connect_daemon()) {
		goto fail;
	}
	bd.session = server->session ? server->session : wlr_session_create(loop);
	if (!bd.session) {
		wlr_log(WLR_ERROR, "basic display: no seat session");
		goto fail;
	}
	struct wlr_backend *multi = server->backend ? server->backend : wlr_multi_backend_create(loop);
	struct wlr_backend *input = wlr_libinput_backend_create(bd.session);
	bd.headless = wlr_headless_backend_create(loop);
	if (!multi || !input || !bd.headless || !wlr_multi_backend_add(multi, input) ||
		!wlr_multi_backend_add(multi, bd.headless)) {
		wlr_log(WLR_ERROR, "basic display: cannot make the backends");
		goto fail;
	}
	server->backend = multi;
	server->session = bd.session;
	bd.active.notify = handle_active;
	wl_signal_add(&bd.session->events.active, &bd.active);
	/* software rendering: there is no GPU to render on; and every frame
	 * composed into our own (readable) buffers, never a client's buffer
	 * handed straight to the output */
	setenv("WLR_RENDERER", "pixman", 1);
	setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);
	bd.on = true;
	return true;

fail:
	if (bd.frame) {
		munmap(bd.frame, (size_t)bd.width * bd.height * 4);
		bd.frame = NULL;
	}
	if (bd.sock >= 0) {
		close(bd.sock);
		bd.sock = -1;
	}
	if (bd.session && !server->session) {
		wlr_session_destroy(bd.session);
	}
	bd.session = NULL;
	return false;
}

void
basic_display_start(struct cg_server *server)
{
	if (!bd.on) {
		return;
	}
	bd.output = wlr_headless_add_output(bd.headless, bd.width, bd.height);
	if (!bd.output) {
		wlr_log(WLR_ERROR, "basic display: cannot add the output");
		return;
	}
	wlr_output_set_description(bd.output, "Basic Display Adapter (firmware framebuffer)");
	bd.commit.notify = handle_commit;
	wl_signal_add(&bd.output->events.commit, &bd.commit);
	bd.output_destroy.notify = handle_output_destroy;
	wl_signal_add(&bd.output->events.destroy, &bd.output_destroy);
}
