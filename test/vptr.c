/*
 * vptr -- a virtual pointer for the gates (wlr-virtual-pointer, privileged).
 *
 *   vptr W H STEP...      W x H: the output layout's size (absolute motion)
 *     m X Y               move to X,Y
 *     d                   left button down
 *     u                   left button up
 *     s MS                wait MS milliseconds
 *
 * e.g. "vptr 1280 720 m 100 12 d m 400 212 u" drags from 100,12 to 400,212.
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define BTN_LEFT 0x110

static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct wl_seat *seat;

static void
global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	(void) data;
	if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
		manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	}
	(void) version;
}

static void
global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void) data;
	(void) registry;
	(void) name;
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static uint32_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
pause_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
	nanosleep(&ts, NULL);
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: vptr W H [m X Y | d | u | s MS]...\n");
		return 2;
	}
	uint32_t w = (uint32_t) atoi(argv[1]), h = (uint32_t) atoi(argv[2]);
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "vptr: no Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!manager) {
		fprintf(stderr, "vptr: no virtual pointer manager (not a privileged client?)\n");
		return 1;
	}
	struct zwlr_virtual_pointer_v1 *ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
	wl_display_roundtrip(display);
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "m") && i + 2 < argc) {
			uint32_t x = (uint32_t) atoi(argv[i + 1]), y = (uint32_t) atoi(argv[i + 2]);
			zwlr_virtual_pointer_v1_motion_absolute(ptr, now_ms(), x, y, w, h);
			zwlr_virtual_pointer_v1_frame(ptr);
			i += 2;
		} else if (!strcmp(argv[i], "d") || !strcmp(argv[i], "u")) {
			zwlr_virtual_pointer_v1_button(ptr, now_ms(), BTN_LEFT,
						       argv[i][0] == 'd' ? WL_POINTER_BUTTON_STATE_PRESSED
									 : WL_POINTER_BUTTON_STATE_RELEASED);
			zwlr_virtual_pointer_v1_frame(ptr);
		} else if (!strcmp(argv[i], "s") && i + 1 < argc) {
			wl_display_flush(display);
			pause_ms(atol(argv[++i]));
			continue;
		}
		wl_display_roundtrip(display);
		pause_ms(60);
	}
	zwlr_virtual_pointer_v1_destroy(ptr);
	wl_display_roundtrip(display);
	wl_display_disconnect(display);
	return 0;
}
