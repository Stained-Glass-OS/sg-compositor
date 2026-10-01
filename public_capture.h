/*
 * sg-compositor: screen capture for everyone, the secure screens blanked
 * (public_capture.c).
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef CG_PUBLIC_CAPTURE_H
#define CG_PUBLIC_CAPTURE_H

#include <wayland-server-core.h>

struct cg_server;

/* the global (wlr-screencopy-unstable-v1, version 3), or NULL */
const struct wl_global *public_capture_create(struct cg_server *server);

#endif
