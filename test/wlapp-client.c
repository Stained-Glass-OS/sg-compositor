/*
 * wlapp-client -- a program's native Wayland window, for wlapp-gate.sh: an
 * xdg_toplevel filled with one colour, as Electron 39+, a GTK 4 or a Qt 6
 * program opens when it decides on Wayland.
 *
 *   wlapp-client [-v VERSION] [-c RRGGBB] [-s WxH] [-m] [-f] [-a APP_ID] [-t TITLE] [-r]
 *     -v  the xdg_wm_base version to bind (default: the compositor's)
 *     -s  its own size, used when the compositor leaves it the choice
 *     -m  asks to be maximized, -f full screen, before it maps
 *     -r  a button press resizes it from its bottom-right corner, not moves it
 * A button press over it asks the compositor to move it (its own title bar,
 * as a client-side decorated window's). It prints each configure ("configure
 * W H [maximized] [fullscreen]") and "closed" when asked to close (then exits).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static uint32_t colour = 0xff0000ff;
static int own_w = 400, own_h = 300, cur_w, cur_h, want_version;
static bool resize_on_press, closed;

static void
global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	(void) data;
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		uint32_t v = version;
		if (want_version > 0 && (uint32_t) want_version < v) {
			v = (uint32_t) want_version;
		}
		if (v > (uint32_t) xdg_wm_base_interface.version) {
			v = (uint32_t) xdg_wm_base_interface.version;
		}
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, v);
	}
}

static void
global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void) data;
	(void) registry;
	(void) name;
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static void
ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void) data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {ping};

static void
draw(int w, int h)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlapp", 0);
	uint32_t *px;
	struct wl_shm_pool *pool;

	if (fd < 0 || ftruncate(fd, size) < 0) {
		exit(2);
	}
	px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		exit(2);
	}
	for (int i = 0; i < w * h; i++) {
		px[i] = colour;
	}
	munmap(px, size);
	pool = wl_shm_create_pool(shm, fd, size);
	if (buffer) {
		wl_buffer_destroy(buffer);
	}
	buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, w, h);
	wl_surface_commit(surface);
	cur_w = w;
	cur_h = h;
}

static int pending_w, pending_h;

static void
xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void) data;
	xdg_surface_ack_configure(xs, serial);
	int w = pending_w > 0 ? pending_w : (cur_w ? cur_w : own_w);
	int h = pending_h > 0 ? pending_h : (cur_h ? cur_h : own_h);
	draw(w, h);
}

static const struct xdg_surface_listener xdg_surface_listener = {xdg_surface_configure};

static void
toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h, struct wl_array *states)
{
	uint32_t *s;
	bool max = false, full = false;
	(void) data;
	(void) tl;
	wl_array_for_each (s, states) {
		max |= *s == XDG_TOPLEVEL_STATE_MAXIMIZED;
		full |= *s == XDG_TOPLEVEL_STATE_FULLSCREEN;
	}
	pending_w = w;
	pending_h = h;
	if (!max && !full && w == 0 && h == 0) {
		pending_w = own_w;
		pending_h = own_h;
	}
	printf("configure %d %d%s%s\n", w, h, max ? " maximized" : "", full ? " fullscreen" : "");
	fflush(stdout);
}

static void
toplevel_close(void *data, struct xdg_toplevel *tl)
{
	(void) data;
	(void) tl;
	printf("closed\n");
	fflush(stdout);
	closed = true;
}

static void
toplevel_bounds(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h)
{
	(void) data;
	(void) tl;
	printf("bounds %d %d\n", w, h);
	fflush(stdout);
}

static void
toplevel_caps(void *data, struct xdg_toplevel *tl, struct wl_array *caps)
{
	(void) data;
	(void) tl;
	(void) caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_bounds,
	.wm_capabilities = toplevel_caps,
};

static void
p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void) d, (void) p, (void) serial, (void) s, (void) x, (void) y;
}
static void
p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void) d, (void) p, (void) serial, (void) s;
}
static void
p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void) d, (void) p, (void) t, (void) x, (void) y;
}
static void
p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
	(void) d, (void) p, (void) t, (void) button;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (resize_on_press) {
			xdg_toplevel_resize(toplevel, seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
		} else {
			xdg_toplevel_move(toplevel, seat, serial);
		}
		printf("pressed\n");
		fflush(stdout);
	}
}
static void
p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
	(void) d, (void) p, (void) t, (void) axis, (void) v;
}
static const struct wl_pointer_listener pointer_listener = {p_enter, p_leave, p_motion, p_button, p_axis};

static struct wl_pointer *pointer;

static void
seat_caps(void *d, struct wl_seat *st, uint32_t caps)
{
	(void) d;
	/* each of the gate's virtual pointers comes and goes: a new wl_pointer each time */
	if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(st);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void
seat_name(void *d, struct wl_seat *st, const char *name)
{
	(void) d, (void) st, (void) name;
}

static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

static uint32_t
parse_colour(const char *s)
{
	return 0xff000000u | (uint32_t) strtoul(s, NULL, 16);
}

int
main(int argc, char **argv)
{
	const char *app_id = "org.example.WlApp", *title = "Wayland test window";
	bool maximize = false, fullscreen = false;
	int opt;

	while ((opt = getopt(argc, argv, "v:c:s:mfa:t:r")) != -1) {
		switch (opt) {
		case 'v': want_version = atoi(optarg); break;
		case 'c': colour = parse_colour(optarg); break;
		case 's': sscanf(optarg, "%dx%d", &own_w, &own_h); break;
		case 'm': maximize = true; break;
		case 'f': fullscreen = true; break;
		case 'a': app_id = optarg; break;
		case 't': title = optarg; break;
		case 'r': resize_on_press = true; break;
		default: return 2;
		}
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "no display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	if (seat) {
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, app_id);
	xdg_toplevel_set_title(toplevel, title);
	if (maximize) {
		xdg_toplevel_set_maximized(toplevel);
	}
	if (fullscreen) {
		xdg_toplevel_set_fullscreen(toplevel, NULL);
	}
	wl_surface_commit(surface);
	printf("version %u\n", wl_proxy_get_version((struct wl_proxy *) wm_base));
	fflush(stdout);
	while (!closed && wl_display_dispatch(display) != -1) {
	}
	return closed ? 0 : 1;
}
