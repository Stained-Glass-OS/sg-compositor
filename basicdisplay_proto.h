#ifndef SG_BASICDISPLAY_PROTO_H
#define SG_BASICDISPLAY_PROTO_H

/* The Basic Display Adapter's socket (sg-basic-displayd <-> sg-compositor).
 *
 * The daemon (root) owns the firmware framebuffer; a compositor in the active
 * session on seat0 draws through it and never holds the framebuffer itself,
 * so no session can read what another session's compositor shows there.
 *
 *   daemon -> client  struct sg_bd_hello, once, on connecting (or the
 *                     connection is closed: not seat0, no framebuffer)
 *   client -> daemon  SG_BD_FRAME with a memfd (SCM_RIGHTS): width*height*4
 *                     bytes of XRGB8888, rows of width*4, sealed against
 *                     shrinking (F_SEAL_SHRINK)
 *   client -> daemon  SG_BD_DAMAGE: up to SG_BD_MAX_RECTS boxes now drawn in
 *                     the memfd; the daemon copies them to the screen while
 *                     the client's session is the active one, and the whole
 *                     frame when its session becomes active again.
 *
 * SOCK_SEQPACKET: each message is one packet.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#define SG_BD_SOCKET "/run/sg-basic-display.sock"
#define SG_BD_MAGIC 0x53474244u /* "SGBD" */
#define SG_BD_MAX_RECTS 64

enum { SG_BD_FRAME = 1, SG_BD_DAMAGE = 2 };

struct sg_bd_hello {
	uint32_t magic;
	uint32_t width, height;
	uint32_t bpp;
	char id[16]; /* the framebuffer driver: "EFI VGA", "VESA VGA" */
};

struct sg_bd_rect {
	int32_t x, y, w, h;
};

struct sg_bd_msg {
	uint32_t type;
	uint32_t count;
	struct sg_bd_rect rects[SG_BD_MAX_RECTS];
};

#endif
