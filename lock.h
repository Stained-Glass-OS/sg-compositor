/*
 * sg-compositor: lock mode and privileged clients (ADR 0009, ADR 0011).
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef CG_LOCK_H
#define CG_LOCK_H

#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>

struct cg_server;
struct cg_view;

struct cg_lock {
	struct cg_server *server;
	bool locked;
	/* The isolation is engaged for a secure prompt (elevation consent,
	 * ADR 0012), not a lock: same visibility and focus rules, but watchers
	 * are told "secure", so the lock service does not put up a lock screen,
	 * and RELEASE -- not UNLOCK -- ends it. A LOCK while secure turns it
	 * into a real lock, which RELEASE will then not undo. */
	bool secure;

	/* Who may unlock and who may connect on the privileged socket: the
	 * machine session's account (sgsystem) and root. Taken from the kernel
	 * via SO_PEERCRED, never from anything a client says. */
	bool have_lock_uid;
	uid_t lock_uid;

	int lock_fd;
	struct wl_event_source *lock_source;
	int control_fd;
	struct wl_event_source *control_source;

	struct wl_list privileged; /* struct cg_privileged_client::link */

	/* Globals only privileged clients may bind: screen capture, input
	 * injection, output and gamma control. */
	const struct wl_global *restricted[16];
	int n_restricted;

	/* WATCH connections: told "locked" / "unlocked" as it happens, so the
	 * lock service can put up the lock screen when Win+L is pressed. */
	struct wl_list watchers; /* struct cg_watcher::link */
};

bool lock_init(struct cg_lock *lock, struct cg_server *server, const char *lock_socket,
	       const char *control_socket, const char *lock_uid);
void lock_fini(struct cg_lock *lock);

/* Clients that connected on the privileged socket. */
bool lock_client_is_privileged(struct cg_lock *lock, const struct wl_client *client);
bool lock_view_is_privileged(struct cg_lock *lock, struct cg_view *view);

/* May this view be seen, focused or receive input right now? While unlocked,
 * yes for everything; while locked, only privileged views. */
bool lock_view_allowed(struct cg_lock *lock, struct cg_view *view);

/* A view was just mapped. */
void lock_view_mapped(struct cg_lock *lock, struct cg_view *view);

/* Offer this global only to privileged clients. */
void lock_restrict_global(struct cg_lock *lock, const struct wl_global *global);

void lock_engage(struct cg_lock *lock);
void lock_release(struct cg_lock *lock);
bool lock_secure_engage(struct cg_lock *lock);
void lock_secure_release(struct cg_lock *lock);

#endif
