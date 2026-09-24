/*
 * sg-compositor: lock mode and privileged clients.
 *
 * While the machine is locked, exactly one class of client may be seen or
 * receive input: those that connected on the privileged socket, which is
 * where the lock screen lives (ADR 0009). Everything else -- in particular
 * the user session's XWayland -- is hidden and gets no keyboard or pointer
 * focus, so it is sent no input events at all.
 *
 * That is the load-bearing property. The adversarial gate showed that freezing
 * the user session is not enough on its own: a frozen program polling
 * GetAsyncKeyState recovers the keys pressed while it was frozen, because the
 * press bits accumulate in the X server's state. So the events must never
 * reach the user's X server. Keyboard focus decides where they go, and only
 * the compositor decides keyboard focus.
 *
 * Authority comes from SO_PEERCRED, the kernel's statement of who is on the
 * other end of a socket. The compositor runs as the logged-in user, so file
 * permissions cannot keep that user's own programs away from its sockets; the
 * peer uid can. Anyone in the session may LOCK. Only the machine session's
 * account or root may UNLOCK, or connect as a privileged client.
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "lock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "seat.h"
#include "server.h"
#include "view.h"

/* Security events are logged at ERROR so they appear in a release build,
 * which logs nothing below it: a refused unlock or a refused privileged
 * client is exactly what an administrator needs to see. */
#define AUDIT(...) wlr_log(WLR_ERROR, "audit: " __VA_ARGS__)

struct cg_watcher {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
};

static void
watcher_free(struct cg_watcher *w)
{
	wl_list_remove(&w->link);
	wl_event_source_remove(w->source);
	close(w->fd);
	free(w);
}

/* A watcher's only traffic is what we send; anything readable means it hung
 * up or misbehaved, and either way it is done. */
static int
handle_watcher_event(int fd, uint32_t mask, void *data)
{
	(void) fd;
	(void) mask;
	watcher_free(data);
	return 0;
}

static void
notify_watchers(struct cg_lock *lock, const char *event)
{
	struct cg_watcher *w, *tmp;
	wl_list_for_each_safe (w, tmp, &lock->watchers, link) {
		if (send(w->fd, event, strlen(event), MSG_NOSIGNAL) < 0) {
			watcher_free(w);
		}
	}
}

struct cg_privileged_client {
	struct wl_list link;
	struct wl_client *client;
	struct wl_listener destroy;
};

static bool
peer_uid(int fd, uid_t *uid)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
		return false;
	}
	*uid = cred.uid;
	return true;
}

/* The machine session's account, or root. */
static bool
uid_may_unlock(struct cg_lock *lock, uid_t uid)
{
	return uid == 0 || (lock->have_lock_uid && uid == lock->lock_uid);
}

/* Anyone in this session may lock: the session's own user, the machine
 * session, root. */
static bool
uid_may_lock(struct cg_lock *lock, uid_t uid)
{
	return uid == getuid() || uid_may_unlock(lock, uid);
}

static int
listen_on(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int fd;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		wlr_log(WLR_ERROR, "socket path too long: %s", path);
		return -1;
	}
	strcpy(addr.sun_path, path);
	unlink(path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		return -1;
	}
	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
		wlr_log_errno(WLR_ERROR, "cannot listen on %s", path);
		close(fd);
		return -1;
	}
	/* Reachable by anyone who can reach the path: the peer-uid checks below
	 * are the authority, not this mode. */
	chmod(path, 0666);
	return fd;
}

static void
privileged_destroy(struct wl_listener *listener, void *data)
{
	struct cg_privileged_client *pc = wl_container_of(listener, pc, destroy);
	(void) data;
	wl_list_remove(&pc->link);
	wl_list_remove(&pc->destroy.link);
	free(pc);
}

static int
handle_lock_connection(int fd, uint32_t mask, void *data)
{
	struct cg_lock *lock = data;
	int client_fd;
	uid_t uid = (uid_t) -1; /* reported as -1 if the kernel cannot say */

	(void) fd;
	(void) mask;
	if ((client_fd = accept4(lock->lock_fd, NULL, NULL, SOCK_CLOEXEC)) < 0) {
		return 0;
	}
	if (!peer_uid(client_fd, &uid) || !uid_may_unlock(lock, uid)) {
		AUDIT("privileged socket refused uid %d", (int) uid);
		close(client_fd);
		return 0;
	}

	struct wl_client *client = wl_client_create(lock->server->wl_display, client_fd);
	if (!client) {
		close(client_fd);
		return 0;
	}
	struct cg_privileged_client *pc = calloc(1, sizeof(*pc));
	if (!pc) {
		wl_client_destroy(client);
		return 0;
	}
	pc->client = client;
	pc->destroy.notify = privileged_destroy;
	wl_client_add_destroy_listener(client, &pc->destroy);
	wl_list_insert(&lock->privileged, &pc->link);
	AUDIT("privileged socket accepted uid %d", (int) uid);
	return 0;
}

/* One command per connection: LOCK, UNLOCK, SECURE, RELEASE, WATCH or STATUS. */
static int
handle_control_connection(int fd, uint32_t mask, void *data)
{
	struct cg_lock *lock = data;
	char buf[64] = {0};
	const char *reply;
	int client_fd;
	uid_t uid = (uid_t) -1; /* reported as -1 if the kernel cannot say */
	ssize_t n;

	(void) fd;
	(void) mask;
	if ((client_fd = accept4(lock->control_fd, NULL, NULL, SOCK_CLOEXEC)) < 0) {
		return 0;
	}
	/* The request is tiny and sent at once; a client that dawdles gets
	 * nothing rather than stalling the compositor. */
	struct timeval tv = {.tv_sec = 1};
	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (!peer_uid(client_fd, &uid)) {
		close(client_fd);
		return 0;
	}
	n = read(client_fd, buf, sizeof(buf) - 1);
	if (n > 0 && buf[n - 1] == '\n') {
		buf[n - 1] = 0;
	}

	if (!strcmp(buf, "LOCK")) {
		if (uid_may_lock(lock, uid)) {
			lock_engage(lock);
			reply = "OK locked\n";
		} else {
			AUDIT("refused LOCK from uid %d", (int) uid);
			reply = "ERR not permitted\n";
		}
	} else if (!strcmp(buf, "UNLOCK")) {
		if (uid_may_unlock(lock, uid)) {
			lock_release(lock);
			reply = "OK unlocked\n";
		} else {
			AUDIT("refused UNLOCK from uid %d", (int) uid);
			reply = "ERR not permitted\n";
		}
	} else if (!strcmp(buf, "WATCH")) {
		struct cg_watcher *w;
		if (!uid_may_unlock(lock, uid) || !(w = calloc(1, sizeof(*w)))) {
			reply = "ERR not permitted\n";
		} else {
			fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);
			w->fd = client_fd;
			w->source = wl_event_loop_add_fd(wl_display_get_event_loop(lock->server->wl_display), client_fd,
							 WL_EVENT_READABLE, handle_watcher_event, w);
			wl_list_insert(&lock->watchers, &w->link);
			reply = lock->locked ? "OK locked\n" : "OK unlocked\n";
			if (send(client_fd, reply, strlen(reply), MSG_NOSIGNAL) < 0) {
				watcher_free(w);
			}
			return 0; /* the connection stays open */
		}
	} else if (!strcmp(buf, "SECURE")) {
		if (!uid_may_unlock(lock, uid)) {
			AUDIT("refused SECURE from uid %d", (int) uid);
			reply = "ERR not permitted\n";
		} else if (lock_secure_engage(lock)) {
			reply = "OK secure\n";
		} else {
			reply = "ERR locked\n";
		}
	} else if (!strcmp(buf, "RELEASE")) {
		if (!uid_may_unlock(lock, uid)) {
			AUDIT("refused RELEASE from uid %d", (int) uid);
			reply = "ERR not permitted\n";
		} else {
			lock_secure_release(lock);
			reply = lock->locked ? "OK locked\n" : "OK unlocked\n";
		}
	} else if (!strcmp(buf, "STATUS")) {
		reply = lock->secure ? "OK secure\n" : lock->locked ? "OK locked\n" : "OK unlocked\n";
	} else {
		reply = "ERR unknown command\n";
	}
	if (send(client_fd, reply, strlen(reply), MSG_NOSIGNAL) < 0) {
		/* nothing useful to do */
	}
	close(client_fd);
	return 0;
}

bool
lock_client_is_privileged(struct cg_lock *lock, const struct wl_client *client)
{
	struct cg_privileged_client *pc;
	wl_list_for_each (pc, &lock->privileged, link) {
		if (pc->client == client) {
			return true;
		}
	}
	return false;
}

bool
lock_view_is_privileged(struct cg_lock *lock, struct cg_view *view)
{
	if (!view || !view->wlr_surface || !view->wlr_surface->resource) {
		return false;
	}
	return lock_client_is_privileged(lock, wl_resource_get_client(view->wlr_surface->resource));
}

bool
lock_view_allowed(struct cg_lock *lock, struct cg_view *view)
{
	return !lock->locked || lock_view_is_privileged(lock, view);
}

static void
view_set_visible(struct cg_view *view, bool visible)
{
	if (view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, visible);
	}
}

void
lock_view_mapped(struct cg_lock *lock, struct cg_view *view)
{
	view_set_visible(view, lock_view_allowed(lock, view));
}

static void
isolate(struct cg_lock *lock, const char *event)
{
	struct cg_server *server = lock->server;
	struct wlr_seat *wlr_seat = server->seat->seat;
	struct cg_view *view, *lock_view = NULL;

	lock->locked = true;
	notify_watchers(lock, event);

	wl_list_for_each (view, &server->views, link) {
		bool privileged = lock_view_is_privileged(lock, view);
		view_set_visible(view, privileged);
		if (privileged && !lock_view) {
			lock_view = view;
		}
	}

	/* Take focus away from everything first. Clearing keyboard focus sends
	 * the client a leave event, so it releases any keys it thinks are held
	 * and receives nothing further. */
	wlr_seat_keyboard_notify_clear_focus(wlr_seat);
	wlr_seat_pointer_notify_clear_focus(wlr_seat);
	if (lock_view) {
		seat_set_focus(server->seat, lock_view);
	}
}

void
lock_engage(struct cg_lock *lock)
{
	if (lock->locked && lock->secure) {
		/* Locked during a secure prompt: it becomes a real lock. The
		 * prompt's own view stays visible (it is privileged); the lock
		 * service now puts its screen up alongside. */
		lock->secure = false;
		AUDIT("locked (during a secure prompt)");
		notify_watchers(lock, "locked\n");
		return;
	}
	if (lock->locked) {
		return;
	}
	AUDIT("locked");
	isolate(lock, "locked\n");
}

bool
lock_secure_engage(struct cg_lock *lock)
{
	if (lock->locked) {
		return false; /* no consent prompt over a locked machine */
	}
	lock->secure = true;
	AUDIT("secure prompt engaged");
	isolate(lock, "secure\n");
	return true;
}

static void
unisolate(struct cg_lock *lock, const char *event)
{
	struct cg_server *server = lock->server;
	struct cg_view *view;

	lock->locked = false;
	lock->secure = false;
	notify_watchers(lock, event);

	wlr_seat_keyboard_notify_clear_focus(server->seat->seat);
	wl_list_for_each (view, &server->views, link) {
		view_set_visible(view, true);
	}
	/* Give focus back to the most recent ordinary view. */
	wl_list_for_each (view, &server->views, link) {
		if (!lock_view_is_privileged(lock, view)) {
			seat_set_focus(server->seat, view);
			break;
		}
	}
}

void
lock_release(struct cg_lock *lock)
{
	if (!lock->locked) {
		return;
	}
	AUDIT("unlocked");
	unisolate(lock, "unlocked\n");
}

void
lock_secure_release(struct cg_lock *lock)
{
	if (!lock->locked || !lock->secure) {
		return; /* not ours to end: unlocked already, or turned into a lock */
	}
	AUDIT("secure prompt released");
	unisolate(lock, "released\n");
}

void
lock_restrict_global(struct cg_lock *lock, const struct wl_global *global)
{
	if (global && lock->n_restricted < (int) (sizeof(lock->restricted) / sizeof(lock->restricted[0]))) {
		lock->restricted[lock->n_restricted++] = global;
	}
}

/* Decides which globals a client can even see. cage offered screen capture
 * (screencopy, export-dmabuf) and input injection (virtual keyboard and
 * pointer) to every client, so any program in the session could photograph
 * the lock screen or type into it. Those, plus output and gamma control, are
 * now invisible to everyone but privileged clients. */
static bool
global_filter(const struct wl_client *client, const struct wl_global *global, void *data)
{
	struct cg_lock *lock = data;
	for (int i = 0; i < lock->n_restricted; i++) {
		if (lock->restricted[i] == global) {
			return lock_client_is_privileged(lock, client);
		}
	}
	return true;
}

static bool
parse_uid(const char *s, uid_t *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || *end || v < 0) {
		return false;
	}
	*out = (uid_t) v;
	return true;
}

bool
lock_init(struct cg_lock *lock, struct cg_server *server, const char *lock_socket, const char *control_socket,
	  const char *lock_uid)
{
	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);

	memset(lock, 0, sizeof(*lock));
	lock->server = server;
	lock->lock_fd = lock->control_fd = -1;
	wl_list_init(&lock->privileged);
	wl_list_init(&lock->watchers);
	wl_display_set_global_filter(server->wl_display, global_filter, lock);

	if (lock_uid) {
		if (!parse_uid(lock_uid, &lock->lock_uid)) {
			wlr_log(WLR_ERROR, "bad lock uid: %s", lock_uid);
			return false;
		}
		lock->have_lock_uid = true;
	}
	if (lock_socket) {
		if ((lock->lock_fd = listen_on(lock_socket)) < 0) {
			return false;
		}
		lock->lock_source =
			wl_event_loop_add_fd(loop, lock->lock_fd, WL_EVENT_READABLE, handle_lock_connection, lock);
	}
	if (control_socket) {
		if ((lock->control_fd = listen_on(control_socket)) < 0) {
			return false;
		}
		lock->control_source =
			wl_event_loop_add_fd(loop, lock->control_fd, WL_EVENT_READABLE, handle_control_connection, lock);
	}
	return true;
}

void
lock_fini(struct cg_lock *lock)
{
	if (lock->lock_source) {
		wl_event_source_remove(lock->lock_source);
	}
	if (lock->control_source) {
		wl_event_source_remove(lock->control_source);
	}
	if (lock->lock_fd >= 0) {
		close(lock->lock_fd);
	}
	if (lock->control_fd >= 0) {
		close(lock->control_fd);
	}
}
