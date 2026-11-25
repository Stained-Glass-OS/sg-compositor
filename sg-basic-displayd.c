/*
 * sg-basic-displayd: the Basic Display Adapter's framebuffer owner (root).
 *
 * With no GPU driver, sg-compositor draws in software and shows its frames
 * on the firmware's framebuffer (basicdisplay.c). The framebuffer device is
 * root's: a session's compositor never opens it, since an open framebuffer
 * stays readable after the console is switched to another session (there is
 * no revoke for fbdev as logind has for DRM and input) -- one user could read
 * the next one's screen. Instead each compositor hands this daemon its frame
 * (a sealed memfd) and says what changed; the daemon copies it to the screen
 * only while that compositor's session is the active one on seat0, and the
 * whole frame again when the session comes back to the foreground.
 *
 * Socket-activated (sg-basic-display.socket, SOCK_SEQPACKET, see
 * basicdisplay_proto.h). SG_BD_FB names another framebuffer (tests).
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>

#include "basicdisplay_proto.h"

#define MAX_CLIENTS 8

struct client {
	int fd;
	char *session;
	uint8_t *frame;
	size_t frame_len;
	int was_active;
};

static struct client clients[MAX_CLIENTS];
static uint8_t *fb;
static uint32_t width, height, bpp, stride, bgr;
static struct sg_bd_hello hello;

static void
copy_box(const uint8_t *src, int x, int y, int w, int h)
{
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (w <= 0 || h <= 0 || x >= (int)width || y >= (int)height) {
		return;
	}
	if (x + w > (int)width) {
		w = width - x;
	}
	if (y + h > (int)height) {
		h = height - y;
	}
	for (int row = y; row < y + h; row++) {
		const uint32_t *s = (const uint32_t *)(src + (size_t)row * width * 4) + x;
		uint8_t *d = fb + (size_t)row * stride;
		if (bpp == 32 && !bgr) {
			memcpy(d + x * 4, s, (size_t)w * 4);
		} else if (bpp == 32) {
			uint32_t *d32 = (uint32_t *)d + x;
			for (int i = 0; i < w; i++) {
				d32[i] = (s[i] & 0xff00ff00) | ((s[i] >> 16) & 0xff) | ((s[i] & 0xff) << 16);
			}
		} else if (bpp == 24) {
			uint8_t *d8 = d + x * 3;
			for (int i = 0; i < w; i++) {
				uint32_t p = s[i];
				d8[3 * i + 0] = bgr ? p >> 16 : p;
				d8[3 * i + 1] = p >> 8;
				d8[3 * i + 2] = bgr ? p : p >> 16;
			}
		} else {
			uint16_t *d16 = (uint16_t *)d + x;
			for (int i = 0; i < w; i++) {
				uint32_t p = s[i];
				d16[i] = ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f);
			}
		}
	}
}

static void
drop(struct client *c)
{
	if (c->frame) {
		munmap(c->frame, c->frame_len);
	}
	close(c->fd);
	free(c->session);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

static int
active(struct client *c)
{
	return c->session && sd_session_is_active(c->session) > 0;
}

static void
accept_client(int lfd)
{
	int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (fd < 0) {
		return;
	}
	struct ucred cred;
	socklen_t len = sizeof(cred);
	char *session = NULL, *seat = NULL;
	if (!fb || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
		sd_pid_get_session(cred.pid, &session) < 0 || sd_session_get_seat(session, &seat) < 0 ||
		strcmp(seat, "seat0") != 0) {
		fprintf(stderr, "sg-basic-displayd: refused pid %d (session %s, seat %s)\n", (int)cred.pid,
			session ? session : "-", seat ? seat : "-");
		free(session);
		free(seat);
		close(fd);
		return;
	}
	free(seat);
	struct client *slot = NULL;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		/* one compositor per session: the newest */
		if (clients[i].fd >= 0 && clients[i].session && strcmp(clients[i].session, session) == 0) {
			drop(&clients[i]);
		}
	}
	for (int i = 0; i < MAX_CLIENTS && !slot; i++) {
		if (clients[i].fd < 0) {
			slot = &clients[i];
		}
	}
	if (!slot || send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) != sizeof(hello)) {
		free(session);
		close(fd);
		return;
	}
	slot->fd = fd;
	slot->session = session;
	fprintf(stderr, "sg-basic-displayd: session %s (uid %d) draws\n", session, (int)cred.uid);
}

static void
take_frame(struct client *c, int mfd)
{
	struct stat st;
	size_t need = (size_t)width * height * 4;
	int seals = fcntl(mfd, F_GET_SEALS);
	/* sealed against shrinking: the client cannot truncate it under us */
	if (fstat(mfd, &st) < 0 || (size_t)st.st_size < need || seals < 0 || !(seals & F_SEAL_SHRINK)) {
		fprintf(stderr, "sg-basic-displayd: session %s: frame refused (size or seals)\n", c->session);
		close(mfd);
		return;
	}
	void *p = mmap(NULL, need, PROT_READ, MAP_SHARED, mfd, 0);
	close(mfd);
	if (p == MAP_FAILED) {
		return;
	}
	if (c->frame) {
		munmap(c->frame, c->frame_len);
	}
	c->frame = p;
	c->frame_len = need;
	c->was_active = 0;
}

static void
read_client(struct client *c)
{
	struct sg_bd_msg msg;
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = {&msg, sizeof(msg)};
	struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
	ssize_t n = recvmsg(c->fd, &mh, MSG_CMSG_CLOEXEC);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		drop(c);
		return;
	}
	if (n < 0) {
		return;
	}
	int mfd = -1;
	for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
			memcpy(&mfd, CMSG_DATA(cm), sizeof(int));
		}
	}
	if (n < 8) {
		if (mfd >= 0) {
			close(mfd);
		}
		return;
	}
	if (msg.type == SG_BD_FRAME && mfd >= 0) {
		take_frame(c, mfd);
		return;
	}
	if (mfd >= 0) {
		close(mfd);
	}
	if (msg.type != SG_BD_DAMAGE || !c->frame) {
		return;
	}
	if (!active(c)) {
		c->was_active = 0;
		return;
	}
	if (!c->was_active) {
		copy_box(c->frame, 0, 0, width, height);
		c->was_active = 1;
		return;
	}
	uint32_t count = msg.count;
	if (count > SG_BD_MAX_RECTS || (size_t)n < 8 + count * sizeof(struct sg_bd_rect)) {
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		copy_box(c->frame, msg.rects[i].x, msg.rects[i].y, msg.rects[i].w, msg.rects[i].h);
	}
}

static int
open_fb(void)
{
	const char *path = getenv("SG_BD_FB") ? getenv("SG_BD_FB") : "/dev/fb0";
	int fd = open(path, O_RDWR | O_CLOEXEC);
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "sg-basic-displayd: no framebuffer at %s: %s\n", path, strerror(errno));
		return -1;
	}
	width = var.xres;
	height = var.yres;
	bpp = var.bits_per_pixel;
	stride = fix.line_length;
	bgr = var.red.offset == 0 && bpp != 16;
	if ((bpp != 32 && bpp != 24 && bpp != 16) || width < 320 || height < 200 || width > 16384 ||
		height > 16384 || (size_t)stride * height > fix.smem_len) {
		fprintf(stderr, "sg-basic-displayd: %s is %ux%u at %u bits per pixel: not usable\n", path, width,
			height, bpp);
		return -1;
	}
	fb = mmap(NULL, (size_t)stride * height, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		fb = NULL;
		fprintf(stderr, "sg-basic-displayd: cannot map %s: %s\n", path, strerror(errno));
		return -1;
	}
	hello.magic = SG_BD_MAGIC;
	hello.width = width;
	hello.height = height;
	hello.bpp = bpp;
	memcpy(hello.id, fix.id, sizeof(hello.id) < sizeof(fix.id) ? sizeof(hello.id) : sizeof(fix.id));
	hello.id[sizeof(hello.id) - 1] = 0;
	fprintf(stderr, "sg-basic-displayd: %s (%s) %ux%u, %u bits per pixel\n", path, hello.id, width, height, bpp);
	return 0;
}

int
main(void)
{
	int lfd;
	if (sd_listen_fds(1) == 1) {
		lfd = SD_LISTEN_FDS_START;
	} else {
		/* started by hand (tests): its own socket */
		const char *path = getenv("SG_BD_SOCKET") ? getenv("SG_BD_SOCKET") : SG_BD_SOCKET;
		struct sockaddr_un sa = {.sun_family = AF_UNIX};
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
		lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		unlink(path);
		if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 4) < 0) {
			perror("sg-basic-displayd: socket");
			return 1;
		}
		chmod(path, 0666);
	}
	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
	}
	/* without a framebuffer every client is refused: its compositor fails
	 * as it would have without this daemon */
	open_fb();
	for (;;) {
		struct pollfd pfd[MAX_CLIENTS + 1];
		int idx[MAX_CLIENTS + 1], n = 0;
		pfd[n].fd = lfd;
		pfd[n].events = POLLIN;
		idx[n++] = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				pfd[n].fd = clients[i].fd;
				pfd[n].events = POLLIN;
				idx[n++] = i;
			}
		}
		/* a quarter second: a session coming back to the foreground gets
		 * its whole frame even if its compositor draws nothing new */
		int r = poll(pfd, n, 250);
		if (r < 0 && errno != EINTR) {
			return 1;
		}
		for (int k = 0; k < n && r > 0; k++) {
			if (!pfd[k].revents) {
				continue;
			}
			if (idx[k] < 0) {
				accept_client(lfd);
			} else if (pfd[k].revents & (POLLIN | POLLHUP | POLLERR)) {
				read_client(&clients[idx[k]]);
			}
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			struct client *c = &clients[i];
			if (c->fd < 0 || !c->frame) {
				continue;
			}
			int a = active(c);
			if (a && !c->was_active) {
				copy_box(c->frame, 0, 0, width, height);
			}
			c->was_active = a;
		}
	}
}
