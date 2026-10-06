/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* closefrom */

#include "config.h"

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#if CAGE_HAS_XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#endif

#include "decor.h"
#include "elevated.h"
#include "output.h"
#include "seat.h"
#include "session_x11.h"
#include "server.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

static void drag_icon_update_position(struct cg_drag_icon *drag_icon);

/* sg-compositor: input is activity for the idle notifier (swayidle), and it
 * turns a screen that display power turned off back on. */
static void
seat_notify_activity(struct cg_server *server)
{
	wlr_idle_notifier_v1_notify_activity(server->idle, server->seat->seat);
	output_power_wake(server);
}

/* XDG toplevels may have nested surfaces, such as popup windows for context
 * menus or tooltips. This function tests if any of those are underneath the
 * coordinates lx and ly (in output Layout Coordinates). If so, it sets the
 * surface pointer to that wlr_surface and the sx and sy coordinates to the
 * coordinates relative to that surface's top-left corner.
 *
 * This function iterates over all of our surfaces and attempts to find one
 * under the cursor. If desktop_view_at returns a view, there is also a
 * surface. There cannot be a surface without a view, either. It's both or
 * nothing.
 */
static struct cg_view *
desktop_view_at(struct cg_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;

	/* Walk up the tree until we find a node with a data pointer. When done,
	 * we've found the node representing the view. */
	while (!node->data) {
		if (!node->parent) {
			node = NULL;
			break;
		}

		node = &node->parent->node;
	}

	assert(node != NULL);
	/* sg-compositor: a view that may not be seen right now is not under
	 * the cursor either, so it receives no pointer, touch or tablet input. */
	if (!lock_view_allowed(&server->lock, node->data)) {
		return NULL;
	}
	return node->data;
}

static void
press_cursor_button(struct cg_seat *seat, struct wlr_input_device *device, uint32_t time, uint32_t button,
		    uint32_t state, double lx, double ly)
{
	struct cg_server *server = seat->server;

	if (state == WLR_BUTTON_PRESSED) {
		double sx, sy;
		struct wlr_surface *surface;
		struct cg_view *view = desktop_view_at(server, lx, ly, &surface, &sx, &sy);
		struct cg_view *current = seat_get_focus(seat);
		if (view == current) {
			return;
		}

		/* Focus that client if the button was pressed and
		   it has no open dialogs. */
		/* sg-compositor: nothing may have the focus (the popup that had
		   it is gone) -- a click then focuses what it lands on. */
		if (view && (!current || !view_is_transient_for(current, view))) {
			seat_set_focus(seat, view);
		}
	}
}

static void
update_capabilities(struct cg_seat *seat)
{
	uint32_t caps = 0;

	if (!wl_list_empty(&seat->keyboard_groups)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	/* sg-compositor: a pen is a pointer too -- a tablet PC with its
	 * keyboard (and touchpad) detached has no other */
	if (!wl_list_empty(&seat->pointers) || !wl_list_empty(&seat->tablets)) {
		caps |= WL_SEAT_CAPABILITY_POINTER;
	}
	if (!wl_list_empty(&seat->touch)) {
		caps |= WL_SEAT_CAPABILITY_TOUCH;
	}
	wlr_seat_set_capabilities(seat->seat, caps);

	/* Hide cursor if the seat doesn't have pointer capability. */
	if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
		wlr_cursor_unset_image(seat->cursor);
	} else {
		wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager, DEFAULT_XCURSOR);
	}
}

static void
map_input_device_to_output(struct cg_seat *seat, struct wlr_input_device *device, const char *output_name)
{
	if (!output_name) {
		wlr_log(WLR_INFO, "Input device %s cannot be mapped to an output device\n", device->name);
		return;
	}

	struct cg_output *output;
	wl_list_for_each (output, &seat->server->outputs, link) {
		if (strcmp(output_name, output->wlr_output->name) == 0) {
			wlr_log(WLR_INFO, "Mapping input device %s to output device %s\n", device->name,
				output->wlr_output->name);
			wlr_cursor_map_input_to_output(seat->cursor, device, output->wlr_output);
			return;
		}
	}

	wlr_log(WLR_INFO, "Couldn't map input device %s to an output\n", device->name);
}

static void
handle_touch_destroy(struct wl_listener *listener, void *data)
{
	struct cg_touch *touch = wl_container_of(listener, touch, destroy);
	struct cg_seat *seat = touch->seat;

	wl_list_remove(&touch->link);
	wlr_cursor_detach_input_device(seat->cursor, &touch->touch->base);
	wl_list_remove(&touch->destroy.link);
	free(touch);

	update_capabilities(seat);
}

static void map_builtin_input(struct cg_seat *seat, struct wlr_input_device *device, const char *output_name);

static void
handle_new_touch(struct cg_seat *seat, struct wlr_touch *wlr_touch)
{
	struct cg_touch *touch = calloc(1, sizeof(struct cg_touch));
	if (!touch) {
		wlr_log(WLR_ERROR, "Cannot allocate touch");
		return;
	}

	touch->seat = seat;
	touch->touch = wlr_touch;
	wlr_cursor_attach_input_device(seat->cursor, &wlr_touch->base);

	wl_list_insert(&seat->touch, &touch->link);
	touch->destroy.notify = handle_touch_destroy;
	wl_signal_add(&wlr_touch->base.events.destroy, &touch->destroy);

	map_builtin_input(seat, &wlr_touch->base, wlr_touch->output_name);
}

/* sg-compositor: a pen (a tablet tool -- a Surface's pen through iptsd, a
 * drawing tablet) drives the pointer: it moves the cursor, its tip is the left
 * button, its barrel buttons the right and middle ones. The programs here --
 * Wine's, through Xwayland -- take a mouse; wlroots' tablet protocol would
 * reach native Wayland programs only, and without this the pen did nothing. */
static void
handle_tablet_destroy(struct wl_listener *listener, void *data)
{
	struct cg_tablet *tablet = wl_container_of(listener, tablet, destroy);
	struct cg_seat *seat = tablet->seat;

	wl_list_remove(&tablet->link);
	wlr_cursor_detach_input_device(seat->cursor, &tablet->tablet->base);
	wl_list_remove(&tablet->destroy.link);
	free(tablet);

	update_capabilities(seat);
}

/* sg-compositor: is this the screen built into the machine (a laptop's or a
 * tablet PC's panel)? SG_INTERNAL_OUTPUT names it outright (a test's headless
 * output); otherwise an embedded DisplayPort, LVDS or DSI connector is. */
static bool
output_is_builtin(struct wlr_output *output)
{
	const char *named = getenv("SG_INTERNAL_OUTPUT");
	if (named && *named) {
		return strcmp(output->name, named) == 0;
	}
	return !strncmp(output->name, "eDP", 3) || !strncmp(output->name, "LVDS", 4) ||
	       !strncmp(output->name, "DSI", 3);
}

/* A pen or touch screen built into the machine, which draws on its own
 * screen: every touch screen, and a pen not on USB or Bluetooth (a
 * Surface's, through iptsd; a convertible's I2C digitizer). A USB drawing
 * tablet spans every screen, as a mouse does. */
static bool
input_is_builtin(struct wlr_input_device *device)
{
#ifdef SG_MUTANT_NO_BUILTIN_MAP
	return false;
#endif
	if (device->type == WLR_INPUT_DEVICE_TOUCH) {
		return true;
	}
	if (device->name && (strstr(device->name, "IPTS") || strstr(device->name, "Surface"))) {
		return true;
	}
	if (wlr_input_device_is_libinput(device)) {
		struct libinput_device *li = wlr_libinput_get_device_handle(device);
		unsigned bus = li ? libinput_device_get_id_bustype(li) : 0;
		return bus != 0x03 /* BUS_USB */ && bus != 0x05 /* BUS_BLUETOOTH */;
	}
	return true;
}

/* sg-compositor (Surface Pro 7 with a monitor attached): the pen and the
 * touch screen are on the built-in screen, so a touch lands under the finger
 * -- spread over every screen, a touch at the panel's middle went to the
 * seam between the two. A device whose udev names its output (WL_OUTPUT)
 * keeps that. No built-in screen (a desktop): the whole layout, as before. */
static void
map_builtin_input(struct cg_seat *seat, struct wlr_input_device *device, const char *output_name)
{
	if (output_name) {
		map_input_device_to_output(seat, device, output_name);
		return;
	}
	if (!input_is_builtin(device)) {
		return;
	}
	struct cg_output *output;
	wl_list_for_each (output, &seat->server->outputs, link) {
		if (output_is_builtin(output->wlr_output)) {
			wlr_log(WLR_INFO, "%s is on the built-in screen %s", device->name ? device->name : "(input)",
				output->wlr_output->name);
			wlr_cursor_map_input_to_output(seat->cursor, device, output->wlr_output);
			return;
		}
	}
	wlr_cursor_map_input_to_output(seat->cursor, device, NULL);
}

void
seat_map_builtin_inputs(struct cg_seat *seat)
{
	if (!seat) {
		return;
	}
	struct cg_touch *touch;
	wl_list_for_each (touch, &seat->touch, link) {
		map_builtin_input(seat, &touch->touch->base, touch->touch->output_name);
	}
	struct cg_tablet *tablet;
	wl_list_for_each (tablet, &seat->tablets, link) {
		map_builtin_input(seat, &tablet->tablet->base, NULL);
	}
}

static void
handle_new_tablet(struct cg_seat *seat, struct wlr_tablet *wlr_tablet)
{
#ifdef SG_MUTANT_TABLET
	wlr_log(WLR_DEBUG, "Tablet input is not implemented");
	return;
#endif
	struct cg_tablet *tablet = calloc(1, sizeof(struct cg_tablet));
	if (!tablet) {
		wlr_log(WLR_ERROR, "Cannot allocate tablet");
		return;
	}

	tablet->seat = seat;
	tablet->tablet = wlr_tablet;
	wlr_cursor_attach_input_device(seat->cursor, &wlr_tablet->base);
	/* programs that take a pen get it as one: Xwayland makes it the X
	 * input devices "xwayland-tablet stylus/eraser", with pressure and
	 * tilt valuators */
	tablet->tablet_v2 = wlr_tablet_create(seat->tablet_manager, seat->seat, &wlr_tablet->base);
	map_builtin_input(seat, &wlr_tablet->base, NULL);

	wl_list_insert(&seat->tablets, &tablet->link);
	tablet->destroy.notify = handle_tablet_destroy;
	wl_signal_add(&wlr_tablet->base.events.destroy, &tablet->destroy);
	wlr_log(WLR_INFO, "Pen %s drives the pointer", wlr_tablet->base.name ? wlr_tablet->base.name : "(unnamed)");
}

static void
handle_pointer_destroy(struct wl_listener *listener, void *data)
{
	struct cg_pointer *pointer = wl_container_of(listener, pointer, destroy);
	struct cg_seat *seat = pointer->seat;

	wl_list_remove(&pointer->link);
	wlr_cursor_detach_input_device(seat->cursor, &pointer->pointer->base);
	wl_list_remove(&pointer->destroy.link);
	free(pointer);

	update_capabilities(seat);
}

static void
handle_new_pointer(struct cg_seat *seat, struct wlr_pointer *wlr_pointer)
{
	struct cg_pointer *pointer = calloc(1, sizeof(struct cg_pointer));
	if (!pointer) {
		wlr_log(WLR_ERROR, "Cannot allocate pointer");
		return;
	}

	pointer->seat = seat;
	pointer->pointer = wlr_pointer;
	wlr_cursor_attach_input_device(seat->cursor, &wlr_pointer->base);

	wl_list_insert(&seat->pointers, &pointer->link);
	pointer->destroy.notify = handle_pointer_destroy;
	wl_signal_add(&wlr_pointer->base.events.destroy, &pointer->destroy);

	map_input_device_to_output(seat, &wlr_pointer->base, wlr_pointer->output_name);
}

/* While Remote Desktop has the session, the console's own devices are
 * ignored: only a privileged client's virtual devices reach the session. */
static bool
console_input_ignored(struct cg_seat *seat, struct wlr_pointer *wlr_pointer)
{
	struct cg_pointer *pointer;

	if (!seat->server->remote) {
		return false;
	}
	wl_list_for_each (pointer, &seat->pointers, link) {
		if (pointer->pointer == wlr_pointer) {
			return !pointer->is_virtual;
		}
	}
	return true;
}

static void
handle_virtual_pointer(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_virtual_pointer);
	struct cg_seat *seat = server->seat;
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_virtual_pointer_v1 *pointer = event->new_pointer;
	struct wlr_pointer *wlr_pointer = &pointer->pointer;

	/* We'll want to map the device back to an output later, this is a bit
	 * sub-optimal (we could just keep the suggested_output), but just copy
	 * its name so we do like other devices
	 */
	if (event->suggested_output != NULL) {
		wlr_pointer->output_name = strdup(event->suggested_output->name);
	}
	/* TODO: event->suggested_seat should be checked if we handle multiple seats */
	handle_new_pointer(seat, wlr_pointer);
	struct cg_pointer *cg_pointer;
	wl_list_for_each (cg_pointer, &seat->pointers, link) {
		if (cg_pointer->pointer == wlr_pointer) {
			cg_pointer->is_virtual = true;
		}
	}
	update_capabilities(seat);
}

static void
handle_modifier_event(struct wlr_keyboard *keyboard, struct cg_seat *seat)
{
	wlr_seat_set_keyboard(seat->seat, keyboard);
	wlr_seat_keyboard_notify_modifiers(seat->seat, &keyboard->modifiers);

	seat_notify_activity(seat->server);
}

static bool
handle_keybinding(struct cg_server *server, xkb_keysym_t sym)
{
#ifdef DEBUG
	if (sym == XKB_KEY_Escape) {
		server_terminate(server);
		return true;
	}
#endif
	if (server->allow_vt_switch && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (wlr_backend_is_multi(server->backend)) {
			if (server->session) {
				unsigned vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
				wlr_session_change_vt(server->session, vt);
			}
		}
	} else {
		return false;
	}
	seat_notify_activity(server);
	return true;
}

/* sg-compositor: keys the compositor reserves, and no client ever sees.
 *
 * Win+L locks, as on Windows. Ctrl+Alt+Del is the secure attention sequence:
 * Wine has none, and on Windows its whole point is that no program can
 * intercept or fake it. Here that property comes from the compositor seeing
 * every key before any client does -- a client cannot grab these, cannot
 * swallow them, and, since input injection is privileged, cannot synthesise
 * them either. Win+L locks; Ctrl+Alt+Del puts up the security screen.
 *
 * A consumed press must take its release with it, or the focused client gets
 * a release for a key it never saw pressed -- and learns a reserved key was
 * used. */
#define CG_MAX_CONSUMED 8
static xkb_keycode_t consumed_keys[CG_MAX_CONSUMED];

static bool
take_consumed_release(xkb_keycode_t keycode)
{
	for (int i = 0; i < CG_MAX_CONSUMED; i++) {
		if (consumed_keys[i] == keycode) {
			consumed_keys[i] = 0;
			return true;
		}
	}
	return false;
}

static void
mark_consumed(xkb_keycode_t keycode)
{
	for (int i = 0; i < CG_MAX_CONSUMED; i++) {
		if (!consumed_keys[i]) {
			consumed_keys[i] = keycode;
			return;
		}
	}
}

/* The volume keys: sg-settingsctl (sg-session) steps the default output and
 * plays the chime at the new level, or toggles mute. Windows' shell answers
 * them; nothing here did, and a laptop's volume keys did nothing (David
 * 2026-10-02). Not waited for: the keyboard stays the compositor's. */
#ifndef SG_MUTANT_NO_VOLUME_KEYS
static void
volume_key(const char *verb, const char *dir)
{
	pid_t pid = fork();
	if (pid < 0) {
		return;
	}
	if (pid == 0) {
		if (fork() == 0) {
			setsid();
			closefrom(3);
			if (dir) {
				execlp("sg-settingsctl", "sg-settingsctl", "sound", verb, dir, "--chime", (char *) NULL);
			} else {
				execlp("sg-settingsctl", "sg-settingsctl", "sound", verb, (char *) NULL);
			}
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}
#endif

/* Ctrl+Alt+Backspace: the session's Windows side started again (David
 * 2026-10-03, "to reload the wine software"): SG_RELOAD_COMMAND, which the
 * session gives (sg-session's sg-wine-reload: this account's Windows programs
 * ended, the shell brought back). Not over a lock or a secure prompt. */
#ifndef SG_MUTANT_NO_RELOAD_KEY
static void
reload_key(void)
{
	const char *cmd = getenv("SG_RELOAD_COMMAND");
	pid_t pid;
	if (!cmd || !*cmd || (pid = fork()) < 0) {
		return;
	}
	if (pid == 0) {
		if (fork() == 0) {
			setsid();
			closefrom(3);
			execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}
#endif

static bool
handle_reserved_key(struct cg_seat *seat, uint32_t modifiers, const xkb_keysym_t *syms, int nsyms)
{
	for (int i = 0; i < nsyms; i++) {
#ifndef SG_MUTANT_NO_VOLUME_KEYS
		if (syms[i] == XKB_KEY_XF86AudioRaiseVolume || syms[i] == XKB_KEY_XF86AudioLowerVolume) {
			volume_key("step", syms[i] == XKB_KEY_XF86AudioRaiseVolume ? "up" : "down");
			return true;
		}
		if (syms[i] == XKB_KEY_XF86AudioMute) {
			volume_key("mute-toggle", NULL);
			return true;
		}
#endif
		bool win_l = (modifiers & WLR_MODIFIER_LOGO) && (syms[i] == XKB_KEY_l || syms[i] == XKB_KEY_L);
		bool sas = (modifiers & WLR_MODIFIER_CTRL) && (modifiers & WLR_MODIFIER_ALT) &&
			   (syms[i] == XKB_KEY_Delete || syms[i] == XKB_KEY_KP_Delete);
		if (win_l) {
			lock_engage(&seat->server->lock);
			return true;
		}
#ifndef SG_MUTANT_NO_RELOAD_KEY
		bool reload = (syms[i] == XKB_KEY_Terminate_Server) ||
			      ((modifiers & WLR_MODIFIER_CTRL) && (modifiers & WLR_MODIFIER_ALT) && syms[i] == XKB_KEY_BackSpace);
		if (reload && !seat->server->lock.locked && !seat->server->lock.secure && !seat->server->lock.sas) {
			reload_key();
			return true;
		}
#endif
		if (sas) {
			/* the security screen; over a lock or a prompt, nothing */
			lock_sas_engage(&seat->server->lock);
			return true;
		}
	}
	return false;
}

static bool
keysyms_have(const xkb_keysym_t *syms, int nsyms, xkb_keysym_t sym)
{
	for (int i = 0; i < nsyms; i++) {
		if (syms[i] == sym || (sym == XKB_KEY_Tab && syms[i] == XKB_KEY_ISO_Left_Tab)) {
			return true;
		}
	}
	return false;
}

static void
handle_key_event(struct wlr_keyboard *keyboard, struct cg_seat *seat, void *data, bool is_virtual)
{
	struct wlr_keyboard_key_event *event = data;

	/* Translate from libinput keycode to an xkbcommon keycode. */
	xkb_keycode_t keycode = event->keycode + 8;

	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);

	/* Remote Desktop has the session: the console's keyboard reaches
	 * nothing, except Ctrl+Alt+Del, which gives the session back to the
	 * console -- locked, so whoever is there must still sign in. */
	if (seat->server->remote && !is_virtual) {
		uint32_t mods = wlr_keyboard_get_modifiers(keyboard);
		for (int i = 0; event->state == WL_KEYBOARD_KEY_STATE_PRESSED && i < nsyms; i++) {
			if ((mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT) &&
			    (syms[i] == XKB_KEY_Delete || syms[i] == XKB_KEY_KP_Delete)) {
				remote_detach(seat->server, "Ctrl+Alt+Del at the console");
				mark_consumed(keycode);
				break;
			}
		}
		return;
	}

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard);
	if (seat->held_press_keycode) {
		/* a shell key's press, held while the focus moved (below): it goes
		 * before this event, to the window that has the keyboard now */
		uint32_t held = seat->held_press_keycode;
		seat->held_press_keycode = 0;
		wlr_seat_set_keyboard(seat->seat, keyboard);
		wlr_seat_keyboard_notify_key(seat->seat, event->time_msec, held, WL_KEYBOARD_KEY_STATE_PRESSED);
	}
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && take_consumed_release(keycode)) {
		handled = true;
		if (seat->switch_keycode == keycode) {
			/* Switched on the Tab's release, not its press: the
			 * session must not be handed a Tab it will never see
			 * released (it would repeat for ever). */
			seat->switch_keycode = 0;
			elevated_focus_session(seat->server);
		}
	} else if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
		   handle_reserved_key(seat, modifiers, syms, nsyms)) {
		mark_consumed(keycode);
		handled = true;
	} else if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
		   seat_get_focus(seat) && seat_get_focus(seat)->elevated && keysyms_have(syms, nsyms, XKB_KEY_Tab)) {
		/* sg-compositor: Alt+Tab in an elevated window goes back to the
		 * session, whose own switcher then lists every window. The
		 * elevated display has no switcher of its own. */
		mark_consumed(keycode);
		seat->switch_keycode = keycode; /* on its release, below */
		handled = true;
	} else if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If Alt is held down and this button was pressed, we
		 * attempt to process it as a compositor
		 * keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(seat->server, syms[i]);
		}
	}

	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
	    (keysyms_have(syms, nsyms, XKB_KEY_Super_L) || keysyms_have(syms, nsyms, XKB_KEY_Super_R) ||
#ifndef SG_MUTANT_HELD_WIN_TO_PROGRAM
	     /* a key with the Windows key already held: the shell's too. Ctrl+Win+
	      * Right held down across the desktops stopped at a Linux program's
	      * full-screen window -- the next Right went to it (David 2026-10-04) */
	     (modifiers & WLR_MODIFIER_LOGO) ||
#endif
	     ((modifiers & WLR_MODIFIER_ALT) && keysyms_have(syms, nsyms, XKB_KEY_Tab)) ||
	     ((modifiers & WLR_MODIFIER_CTRL) && (modifiers & WLR_MODIFIER_SHIFT) &&
	      keysyms_have(syms, nsyms, XKB_KEY_Escape)))) {
		/* the Windows key, Alt+Tab and Ctrl+Shift+Esc are the shell's:
		 * from a Linux program's window, the desktop takes the keyboard
		 * first (session_x11.c) */
		seat->enter_skip_keycode = event->keycode;
		if (session_x11_super(seat->server, seat_get_focus(seat))) {
#ifndef SG_MUTANT_SUPER_RACE
			/* its press goes with the next key event, not now: X's focus
			 * moves with a request of its own, and a press sent with
			 * it was lost -- only the release reached the shell, and
			 * Start did not open */
			seat->held_press_keycode = event->keycode;
			handled = true;
#endif
		}
		seat->enter_skip_keycode = 0;
	}
	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* sg-compositor: the user typing into an elevated window is
		 * what lets the session's clipboard text reach it. */
		elevated_user_input(seat_get_focus(seat));
	}
	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat->seat, keyboard);
		wlr_seat_keyboard_notify_key(seat->seat, event->time_msec, event->keycode, event->state);
	}

	seat_notify_activity(seat->server);
}

static void
handle_keyboard_group_key(struct wl_listener *listener, void *data)
{
	struct cg_keyboard_group *cg_group = wl_container_of(listener, cg_group, key);
	handle_key_event(&cg_group->wlr_group->keyboard, cg_group->seat, data, cg_group->is_virtual);
}

static void
handle_keyboard_group_modifiers(struct wl_listener *listener, void *data)
{
	struct cg_keyboard_group *group = wl_container_of(listener, group, modifiers);
	handle_modifier_event(&group->wlr_group->keyboard, group->seat);
}

static void
cg_keyboard_group_add(struct wlr_keyboard *keyboard, struct cg_seat *seat, bool virtual)
{
	/* We apparently should not group virtual keyboards,
	 * so create a new group with it
	 */
	if (!virtual) {
		struct cg_keyboard_group *group;
		wl_list_for_each (group, &seat->keyboard_groups, link) {
			if (group->is_virtual)
				continue;
			struct wlr_keyboard_group *wlr_group = group->wlr_group;
			if (wlr_keyboard_group_add_keyboard(wlr_group, keyboard)) {
				wlr_log(WLR_DEBUG, "Added new keyboard to existing group");
				return;
			}
		}
	}

	/* This is reached if and only if the keyboard could not be inserted into
	 * any group */
	struct cg_keyboard_group *cg_group = calloc(1, sizeof(struct cg_keyboard_group));
	if (cg_group == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate keyboard group.");
		return;
	}
	cg_group->seat = seat;
	cg_group->is_virtual = virtual;
	cg_group->wlr_group = wlr_keyboard_group_create();
	if (cg_group->wlr_group == NULL) {
		wlr_log(WLR_ERROR, "Failed to create wlr keyboard group.");
		goto cleanup;
	}

	cg_group->wlr_group->data = cg_group;
	wlr_keyboard_set_keymap(&cg_group->wlr_group->keyboard, keyboard->keymap);

	wlr_keyboard_set_repeat_info(&cg_group->wlr_group->keyboard, keyboard->repeat_info.rate,
				     keyboard->repeat_info.delay);

	wlr_log(WLR_DEBUG, "Created keyboard group");

	wlr_keyboard_group_add_keyboard(cg_group->wlr_group, keyboard);
	wl_list_insert(&seat->keyboard_groups, &cg_group->link);

	wl_signal_add(&cg_group->wlr_group->keyboard.events.key, &cg_group->key);
	cg_group->key.notify = handle_keyboard_group_key;
	wl_signal_add(&cg_group->wlr_group->keyboard.events.modifiers, &cg_group->modifiers);
	cg_group->modifiers.notify = handle_keyboard_group_modifiers;

	return;

cleanup:
	if (cg_group && cg_group->wlr_group) {
		wlr_keyboard_group_destroy(cg_group->wlr_group);
	}
	free(cg_group);
}

static void
handle_new_keyboard(struct cg_seat *seat, struct wlr_keyboard *keyboard, bool virtual)
{
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		wlr_log(WLR_ERROR, "Unable to create XKB context");
		return;
	}

	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "Unable to configure keyboard: keymap does not exist");
		xkb_context_unref(context);
		return;
	}

	wlr_keyboard_set_keymap(keyboard, keymap);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(keyboard, 25, 600);

	cg_keyboard_group_add(keyboard, seat, virtual);

	wlr_seat_set_keyboard(seat->seat, keyboard);
}

static void
handle_virtual_keyboard(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct cg_seat *seat = server->seat;
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	struct wlr_keyboard *wlr_keyboard = &keyboard->keyboard;

	/* TODO: If multiple seats are supported, check keyboard->seat
	 * to select the appropriate one */

	handle_new_keyboard(seat, wlr_keyboard, true);
	update_capabilities(seat);
}

static void
handle_new_input(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		handle_new_keyboard(seat, wlr_keyboard_from_input_device(device), false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		handle_new_pointer(seat, wlr_pointer_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		handle_new_touch(seat, wlr_touch_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		wlr_log(WLR_DEBUG, "Switch input is not implemented");
		return;
	case WLR_INPUT_DEVICE_TABLET:
		handle_new_tablet(seat, wlr_tablet_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		wlr_log(WLR_DEBUG, "Tablet input is not implemented");
		return;
	}

	update_capabilities(seat);
}

static void
handle_request_set_primary_selection(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;

	wlr_seat_set_primary_selection(seat->seat, event->source, event->serial);
}

static void
handle_request_set_selection(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;

	wlr_seat_set_selection(seat->seat, event->source, event->serial);
}

static void
handle_request_set_cursor(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_surface *focused_surface = event->seat_client->seat->pointer_state.focused_surface;
	bool has_focused = focused_surface != NULL && focused_surface->resource != NULL;
	struct wl_client *focused_client = NULL;
	if (has_focused) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	/* This can be sent by any client, so we check to make sure
	 * this one actually has pointer focus first. */
	if (focused_client == event->seat_client->client) {
		wlr_cursor_set_surface(seat->cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

static void
handle_touch_down(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_down);
	if (seat->server->remote) {
		return;
	}
	struct wlr_touch_down_event *event = data;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, &event->touch->base, event->x, event->y, &lx, &ly);

	double sx, sy;
	struct wlr_surface *surface;
	struct cg_view *view = desktop_view_at(seat->server, lx, ly, &surface, &sx, &sy);

	uint32_t serial = 0;
	if (view) {
		serial = wlr_seat_touch_notify_down(seat->seat, surface, event->time_msec, event->touch_id, sx, sy);
	}

	if (serial && wlr_seat_touch_num_points(seat->seat) == 1) {
		seat->touch_id = event->touch_id;
		seat->touch_lx = lx;
		seat->touch_ly = ly;
		press_cursor_button(seat, &event->touch->base, event->time_msec, BTN_LEFT, WLR_BUTTON_PRESSED, lx, ly);
	}

	seat_notify_activity(seat->server);
}

static void
handle_touch_up(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_up);
	if (seat->server->remote) {
		return;
	}
	struct wlr_touch_up_event *event = data;

	if (!wlr_seat_touch_get_point(seat->seat, event->touch_id)) {
		return;
	}

	if (wlr_seat_touch_num_points(seat->seat) == 1) {
		press_cursor_button(seat, &event->touch->base, event->time_msec, BTN_LEFT, WLR_BUTTON_RELEASED,
				    seat->touch_lx, seat->touch_ly);
	}

	wlr_seat_touch_notify_up(seat->seat, event->time_msec, event->touch_id);
	seat_notify_activity(seat->server);
}

static void
handle_touch_motion(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_motion);
	if (seat->server->remote) {
		return;
	}
	struct wlr_touch_motion_event *event = data;

	if (!wlr_seat_touch_get_point(seat->seat, event->touch_id)) {
		return;
	}

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, &event->touch->base, event->x, event->y, &lx, &ly);

	double sx, sy;
	struct wlr_surface *surface;
	struct cg_view *view = desktop_view_at(seat->server, lx, ly, &surface, &sx, &sy);

	if (view) {
		wlr_seat_touch_point_focus(seat->seat, surface, event->time_msec, event->touch_id, sx, sy);
		wlr_seat_touch_notify_motion(seat->seat, event->time_msec, event->touch_id, sx, sy);
	} else {
		wlr_seat_touch_point_clear_focus(seat->seat, event->time_msec, event->touch_id);
	}

	if (event->touch_id == seat->touch_id) {
		seat->touch_lx = lx;
		seat->touch_ly = ly;
	}

	seat_notify_activity(seat->server);
}

static void
handle_touch_frame(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, touch_frame);

	wlr_seat_touch_notify_frame(seat->seat);
	seat_notify_activity(seat->server);
}

static void
handle_cursor_frame(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_frame);

	wlr_seat_pointer_notify_frame(seat->seat);
	seat_notify_activity(seat->server);
}

static void
handle_cursor_axis(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_axis);
	struct wlr_pointer_axis_event *event = data;

	if (console_input_ignored(seat, event->pointer)) {
		return;
	}

	wlr_seat_pointer_notify_axis(seat->seat, event->time_msec, event->orientation, event->delta,
				     event->delta_discrete, event->source, event->relative_direction);
	seat_notify_activity(seat->server);
}

/* A button of the pointer: a mouse's, or a pen's (its tip, its barrel buttons). */
static void
seat_pointer_button(struct cg_seat *seat, struct wlr_input_device *device, uint32_t time_msec, uint32_t button,
		    enum wl_pointer_button_state state)
{
	/* sg-compositor: a click on a Linux program's title bar is the
	 * compositor's (decor.c), not the window's */
#if CAGE_HAS_XWAYLAND
	if (decor_button(seat, state == WL_POINTER_BUTTON_STATE_PRESSED, time_msec)) {
		seat_notify_activity(seat->server);
		return;
	}
#endif
	elevated_grab_button(seat, state == WL_POINTER_BUTTON_STATE_PRESSED);
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		/* sg-compositor: a click on an elevated window, like a key, lets
		 * the session's clipboard text reach it (a menu's Paste). */
		double sx, sy;
		struct wlr_surface *surface;
		elevated_user_input(desktop_view_at(seat->server, seat->cursor->x, seat->cursor->y, &surface, &sx, &sy));
	}
	wlr_seat_pointer_notify_button(seat->seat, time_msec, button, state);
	press_cursor_button(seat, device, time_msec, button, state, seat->cursor->x, seat->cursor->y);
	seat_notify_activity(seat->server);
}

static void
handle_cursor_button(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (console_input_ignored(seat, event->pointer)) {
		return;
	}

	seat_pointer_button(seat, &event->pointer->base, event->time_msec, event->button, event->state);
}

int
scale_for_screen(int w, int h)
{
	int s = w < h ? w : h, q;
	if (s <= 0) {
		return 100;
	}
	q = (s * 4 + 540) / 1080;
	return (q < 4 ? 4 : q > 16 ? 16 : q) * 25;
}

/* sg-compositor: the pointer at the display scale (David 2026-10-05: on a
 * Surface Pro 7, 2736x1824, everything was tiny -- the pointer too). The
 * size Settings gives (effects.conf, the user's scale), else, before
 * anyone signs in (the login screen, Setup), the screen's recommended
 * scale's share of 24 px. The X server's own pointer (over a window that
 * sets none) goes with it. */
void
seat_update_cursor_size(struct cg_seat *seat, bool force)
{
	struct timespec now;
	struct wlr_xcursor_manager *mgr;
	struct cg_output *output;
	int size = 0, h = 0, w = 0;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!force && now.tv_sec == seat->cursor_checked.tv_sec) {
		return;
	}
	seat->cursor_checked = now;
#if CAGE_HAS_XWAYLAND
	size = decor_cursor_size();
#endif
	if (!size) {
		wl_list_for_each (output, &seat->server->outputs, link) {
			if (output->wlr_output->height > h) {
				w = output->wlr_output->width;
				h = output->wlr_output->height;
			}
		}
		size = XCURSOR_SIZE * scale_for_screen(w, h) / 100;
	}
#ifdef SG_MUTANT_CURSOR_FIXED
	size = XCURSOR_SIZE;
#endif
	if (size == seat->cursor_size && seat->xcursor_manager && seat->xwayland_cursor) {
		return;
	}
	if (size == seat->cursor_size && seat->xcursor_manager) {
		mgr = seat->xcursor_manager;
		goto xwayland;
	}
	if (!(mgr = wlr_xcursor_manager_create(NULL, size))) {
		return;
	}
	wl_list_for_each (output, &seat->server->outputs, link) {
		wlr_xcursor_manager_load(mgr, output->wlr_output->scale);
	}
	wlr_xcursor_manager_load(mgr, 1);
	if (seat->xcursor_manager) {
		wlr_xcursor_manager_destroy(seat->xcursor_manager);
	}
	seat->xcursor_manager = mgr;
	seat->cursor_size = size;
	wlr_log(WLR_INFO, "pointer size %d", size);
	if (!seat->seat->pointer_state.focused_surface) {
		wlr_cursor_set_xcursor(seat->cursor, mgr, DEFAULT_XCURSOR);
	}
xwayland:
	seat->xwayland_cursor = true;
#if CAGE_HAS_XWAYLAND
	if (seat->server->xwayland) {
		struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(mgr, DEFAULT_XCURSOR, 1);
		if (xcursor) {
			struct wlr_xcursor_image *image = xcursor->images[0];
			wlr_xwayland_set_cursor(seat->server->xwayland, image->buffer, image->width * 4, image->width,
						image->height, image->hotspot_x, image->hotspot_y);
		}
	}
#endif
}

static void
process_cursor_motion(struct cg_seat *seat, uint32_t time_msec, double dx, double dy, double dx_unaccel,
		      double dy_unaccel)
{
	double sx, sy;
	struct wlr_seat *wlr_seat = seat->seat;
	struct wlr_surface *surface = NULL;

	seat_update_cursor_size(seat, false);
	/* sg-compositor: an elevated window being moved or resized by the user
	 * takes the motion; the window under the pointer does not see it. */
	if (elevated_grab_motion(seat, seat->cursor->x, seat->cursor->y)) {
		seat_notify_activity(seat->server);
		return;
	}
	/* and a title bar being dragged takes it too */
#if CAGE_HAS_XWAYLAND
	if (decor_motion(seat, seat->cursor->x, seat->cursor->y)) {
		seat_notify_activity(seat->server);
		return;
	}
#endif

	struct cg_view *view = desktop_view_at(seat->server, seat->cursor->x, seat->cursor->y, &surface, &sx, &sy);
	if (!view) {
		wlr_seat_pointer_clear_focus(wlr_seat);
	} else if (wlr_seat->pointer_state.focused_surface != surface) {
		/* sg-compositor: entering a surface, Xwayland is also given a
		 * motion to it -- wlroots leaves out a motion to where the enter
		 * already put the pointer, and an X window entered that way
		 * missed the click that followed (a click right after the
		 * pointer jumped there: a tablet, a touch screen, Remote
		 * Desktop, the QA VM): its button went to the X root. */
		wlr_seat_pointer_notify_enter(wlr_seat, surface, sx, sy);
		wlr_seat_pointer_notify_frame(wlr_seat);
		wlr_seat_pointer_notify_motion(wlr_seat, time_msec, sx + 1.0 / 128, sy);
		wlr_seat_pointer_notify_motion(wlr_seat, time_msec, sx, sy);
	} else {
		wlr_seat_pointer_notify_motion(wlr_seat, time_msec, sx, sy);
	}

	if (dx != 0 || dy != 0) {
		wlr_relative_pointer_manager_v1_send_relative_motion(seat->server->relative_pointer_manager, wlr_seat,
								     (uint64_t) time_msec * 1000, dx, dy, dx_unaccel,
								     dy_unaccel);
	}

	struct cg_drag_icon *drag_icon;
	wl_list_for_each (drag_icon, &seat->drag_icons, link) {
		drag_icon_update_position(drag_icon);
	}

	seat_notify_activity(seat->server);
}

static void
handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;

	if (console_input_ignored(seat, event->pointer)) {
		return;
	}

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(seat->cursor, &event->pointer->base, event->x, event->y, &lx, &ly);

	double dx = lx - seat->cursor->x;
	double dy = ly - seat->cursor->y;

	wlr_cursor_warp_absolute(seat->cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(seat, event->time_msec, dx, dy, dx, dy);
	seat_notify_activity(seat->server);
}

static void
handle_cursor_motion_relative(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, cursor_motion_relative);
	struct wlr_pointer_motion_event *event = data;

	if (console_input_ignored(seat, event->pointer)) {
		return;
	}

	wlr_cursor_move(seat->cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_cursor_motion(seat, event->time_msec, event->delta_x, event->delta_y, event->unaccel_dx,
			      event->unaccel_dy);
	seat_notify_activity(seat->server);
}

/* The pen moved to X, Y (0..1 across the screen; NAN: that one unchanged). */
static void
tablet_pointer_to(struct cg_seat *seat, struct wlr_input_device *device, uint32_t time_msec, double x, double y)
{
	double ox = seat->cursor->x, oy = seat->cursor->y;
	wlr_cursor_warp_absolute(seat->cursor, device, x, y);
	double dx = seat->cursor->x - ox, dy = seat->cursor->y - oy;
	process_cursor_motion(seat, time_msec, dx, dy, dx, dy);
	wlr_seat_pointer_notify_frame(seat->seat);
}

/* sg-compositor: a pen reaches the programs that take one as a pen
 * (tablet-v2) -- pressure and tilt with it. Xwayland takes one for every X
 * window (Wine's and the Linux programs'): its X input device has pressure
 * and tilt valuators (GTK, Qt, Wine's Wintab and pointer messages read them)
 * and moves and clicks the X pointer as well, so a program that knows no
 * pen still has a mouse. Over nothing that takes a pen (the compositor's
 * title bars and outlines, the background) the pen is the mouse as before. */
static void
handle_tablet_tool_set_cursor(struct wl_listener *listener, void *data)
{
	struct cg_tablet_tool *tool = wl_container_of(listener, tool, set_cursor);
	struct wlr_tablet_v2_event_cursor *event = data;

	/* only the program the pen is over may shape its pointer */
	if (!tool->tool_v2->focused_surface || !event->seat_client ||
	    wl_resource_get_client(tool->tool_v2->focused_surface->resource) != event->seat_client->client) {
		return;
	}
	wlr_cursor_set_surface(tool->seat->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void
handle_tablet_tool_destroy(struct wl_listener *listener, void *data)
{
	struct cg_tablet_tool *tool = wl_container_of(listener, tool, destroy);

	wl_list_remove(&tool->set_cursor.link);
	wl_list_remove(&tool->destroy.link);
	free(tool);
}

static struct cg_tablet_tool *
tablet_tool_get(struct cg_seat *seat, struct wlr_tablet *wlr_tablet, struct wlr_tablet_tool *wlr_tool)
{
	if (wlr_tool->data) {
		return wlr_tool->data;
	}
	struct cg_tablet_tool *tool = calloc(1, sizeof(*tool));
	if (!tool) {
		return NULL;
	}
	tool->tool_v2 = wlr_tablet_tool_create(seat->tablet_manager, seat->seat, wlr_tool);
	if (!tool->tool_v2) {
		free(tool);
		return NULL;
	}
	tool->seat = seat;
	tool->set_cursor.notify = handle_tablet_tool_set_cursor;
	wl_signal_add(&tool->tool_v2->events.set_cursor, &tool->set_cursor);
	tool->destroy.notify = handle_tablet_tool_destroy;
	wl_signal_add(&wlr_tool->events.destroy, &tool->destroy);
	wlr_tool->data = tool;
	return tool;
}

static struct wlr_tablet_v2_tablet *
tablet_v2_of(struct cg_seat *seat, struct wlr_tablet *wlr_tablet)
{
	struct cg_tablet *tablet;
	wl_list_for_each (tablet, &seat->tablets, link) {
		if (tablet->tablet == wlr_tablet) {
			return tablet->tablet_v2;
		}
	}
	return NULL;
}

/* The pen is at the cursor: to the program under it as a pen, if it takes
 * one (true), else nothing sent (false: the caller makes it the mouse). In
 * a stroke (tip down) it stays with the program the stroke began on. */
static bool
tablet_tool_to_program(struct cg_seat *seat, struct wlr_tablet *wlr_tablet, struct cg_tablet_tool *tool)
{
#ifdef SG_MUTANT_TABLET_AS_MOUSE
	return false;
#endif
	struct wlr_tablet_v2_tablet *tablet_v2 = tablet_v2_of(seat, wlr_tablet);
	if (!tool || !tablet_v2 || tool->pointer_down) {
		return false;
	}
	double lx = seat->cursor->x, ly = seat->cursor->y;
	if (tool->tool_v2->is_down && tool->tool_v2->focused_surface) {
		wlr_tablet_v2_tablet_tool_notify_motion(tool->tool_v2, lx - tool->grab_dx, ly - tool->grab_dy);
		return true;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct cg_view *view = desktop_view_at(seat->server, lx, ly, &surface, &sx, &sy);
	if (!view || !surface || !wlr_surface_accepts_tablet_v2(tablet_v2, surface)) {
		if (tool->tool_v2->focused_surface) {
			wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tool_v2);
		}
		return false;
	}
	if (tool->tool_v2->focused_surface != surface) {
		if (tool->tool_v2->focused_surface) {
			wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tool_v2);
		}
		wlr_tablet_v2_tablet_tool_notify_proximity_in(tool->tool_v2, tablet_v2, surface);
		/* the program's own pointer shape, once it sets one */
		wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager, DEFAULT_XCURSOR);
	}
	tool->grab_dx = lx - sx;
	tool->grab_dy = ly - sy;
	wlr_tablet_v2_tablet_tool_notify_motion(tool->tool_v2, sx, sy);
	return true;
}

/* where the pen is now, from an event's X and Y (NAN: unchanged) */
static void
tablet_cursor_to(struct cg_seat *seat, struct wlr_input_device *device, double x, double y)
{
	wlr_cursor_warp_absolute(seat->cursor, device, x, y);
}

static void
handle_tablet_tool_axis(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, tablet_tool_axis);
	struct wlr_tablet_tool_axis_event *event = data;

	if (seat->server->remote) {
		return;
	}
	struct cg_tablet_tool *tool = tablet_tool_get(seat, event->tablet, event->tool);
	bool moved = event->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y);
	if (moved) {
		tablet_cursor_to(seat, &event->tablet->base, event->updated_axes & WLR_TABLET_TOOL_AXIS_X ? event->x : NAN,
				 event->updated_axes & WLR_TABLET_TOOL_AXIS_Y ? event->y : NAN);
	}
	if (tool && tablet_tool_to_program(seat, event->tablet, tool)) {
		struct wlr_tablet_v2_tablet_tool *t = tool->tool_v2;
		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) {
			wlr_tablet_v2_tablet_tool_notify_pressure(t, event->pressure);
		}
		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) {
			wlr_tablet_v2_tablet_tool_notify_distance(t, event->distance);
		}
		if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) {
			if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X) {
				tool->tilt_x = event->tilt_x;
			}
			if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y) {
				tool->tilt_y = event->tilt_y;
			}
			wlr_tablet_v2_tablet_tool_notify_tilt(t, tool->tilt_x, tool->tilt_y);
		}
		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION) {
			wlr_tablet_v2_tablet_tool_notify_rotation(t, event->rotation);
		}
		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER) {
			wlr_tablet_v2_tablet_tool_notify_slider(t, event->slider);
		}
		if (event->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) {
			wlr_tablet_v2_tablet_tool_notify_wheel(t, event->wheel_delta, 0);
		}
		seat_notify_activity(seat->server);
		return;
	}
	if (moved) {
		tablet_pointer_to(seat, &event->tablet->base, event->time_msec, NAN, NAN);
	}
}

/* Whether a pen is in range, for X programs: the root window's
 * _SG_PEN_IN_RANGE (CARDINAL, 1 or 0). Xwayland's pens have no proximity
 * events -- a pen lifted out of range sends X nothing a program can tell from
 * a pen held still -- so Wine (wine-sg 1150) reads this to send its window
 * WM_POINTERLEAVE, as Windows does when a pen leaves the screen's range. */
static void
seat_pen_range(struct cg_server *server, bool in_range)
{
#if CAGE_HAS_XWAYLAND
	static xcb_atom_t atom;
	xcb_connection_t *c;
	uint32_t value = in_range;

#ifdef SG_MUTANT_NO_PEN_RANGE
	return;
#endif
	if (!server->xwayland || !(c = wlr_xwayland_get_xwm_connection(server->xwayland))) {
		return;
	}
	if (!atom) {
		xcb_intern_atom_reply_t *a =
			xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen("_SG_PEN_IN_RANGE"), "_SG_PEN_IN_RANGE"), NULL);
		if (!a) {
			return;
		}
		atom = a->atom;
		free(a);
	}
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, xcb_setup_roots_iterator(xcb_get_setup(c)).data->root, atom,
			    XCB_ATOM_CARDINAL, 32, 1, &value);
	xcb_flush(c);
#else
	(void) server;
	(void) in_range;
#endif
}

static void
handle_tablet_tool_proximity(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, tablet_tool_proximity);
	struct wlr_tablet_tool_proximity_event *event = data;

	if (seat->server->remote) {
		return;
	}
	seat_pen_range(seat->server, event->state == WLR_TABLET_TOOL_PROXIMITY_IN);
	struct cg_tablet_tool *tool = tablet_tool_get(seat, event->tablet, event->tool);
	if (event->state != WLR_TABLET_TOOL_PROXIMITY_IN) {
		if (tool && tool->tool_v2->focused_surface) {
			wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tool_v2);
		}
		return;
	}
	tablet_cursor_to(seat, &event->tablet->base, event->x, event->y);
	if (tablet_tool_to_program(seat, event->tablet, tool)) {
		seat_notify_activity(seat->server);
		return;
	}
	tablet_pointer_to(seat, &event->tablet->base, event->time_msec, NAN, NAN);
}

static void
handle_tablet_tool_tip(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, tablet_tool_tip);
	struct wlr_tablet_tool_tip_event *event = data;

	if (seat->server->remote) {
		return;
	}
	struct cg_tablet_tool *tool = tablet_tool_get(seat, event->tablet, event->tool);
	bool down = event->state == WLR_TABLET_TOOL_TIP_DOWN;
	tablet_cursor_to(seat, &event->tablet->base, event->x, event->y);
	if (tablet_tool_to_program(seat, event->tablet, tool)) {
		if (down) {
			/* a tap of the pen, like a click, brings the window
			 * forward (and lets the session's clipboard reach an
			 * elevated one) */
			double sx, sy;
			struct wlr_surface *surface;
			elevated_user_input(
				desktop_view_at(seat->server, seat->cursor->x, seat->cursor->y, &surface, &sx, &sy));
			press_cursor_button(seat, &event->tablet->base, event->time_msec, BTN_LEFT, WLR_BUTTON_PRESSED,
					    seat->cursor->x, seat->cursor->y);
			wlr_tablet_v2_tablet_tool_notify_down(tool->tool_v2);
			wlr_tablet_tool_v2_start_implicit_grab(tool->tool_v2);
		} else {
			wlr_tablet_v2_tablet_tool_notify_up(tool->tool_v2);
		}
		seat_notify_activity(seat->server);
		return;
	}
	tablet_pointer_to(seat, &event->tablet->base, event->time_msec, NAN, NAN);
	if (tool) {
		tool->pointer_down = down;
	}
	seat_pointer_button(seat, &event->tablet->base, event->time_msec, BTN_LEFT,
			    down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(seat->seat);
}

static void
handle_tablet_tool_button(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, tablet_tool_button);
	struct wlr_tablet_tool_button_event *event = data;

	if (seat->server->remote) {
		return;
	}
	/* the barrel's buttons: the lower is the right button, the upper the
	 * middle -- the pen's buttons on Windows. Xwayland makes BTN_STYLUS X's
	 * button 2 and BTN_STYLUS2 button 3, so to a program taking the pen
	 * they are swapped. */
	uint32_t button = event->button == BTN_STYLUS ? BTN_RIGHT : event->button == BTN_STYLUS2 ? BTN_MIDDLE : 0;
	if (!button) {
		return;
	}
	struct cg_tablet_tool *tool = tablet_tool_get(seat, event->tablet, event->tool);
	if (tool && tool->tool_v2->focused_surface && !tool->pointer_down) {
		wlr_tablet_v2_tablet_tool_notify_button(tool->tool_v2,
							button == BTN_RIGHT ? BTN_STYLUS2 : BTN_STYLUS,
							event->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
											   : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
		seat_notify_activity(seat->server);
		return;
	}
	seat_pointer_button(seat, &event->tablet->base, event->time_msec, button,
			    event->state == WLR_BUTTON_PRESSED ? WL_POINTER_BUTTON_STATE_PRESSED
							       : WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(seat->seat);
}

static void
drag_icon_update_position(struct cg_drag_icon *drag_icon)
{
	struct wlr_drag_icon *wlr_icon = drag_icon->wlr_drag_icon;
	struct cg_seat *seat = drag_icon->seat;
	struct wlr_touch_point *point;

	switch (wlr_icon->drag->grab_type) {
	case WLR_DRAG_GRAB_KEYBOARD:
		return;
	case WLR_DRAG_GRAB_KEYBOARD_POINTER:
		drag_icon->lx = seat->cursor->x;
		drag_icon->ly = seat->cursor->y;
		break;
	case WLR_DRAG_GRAB_KEYBOARD_TOUCH:
		point = wlr_seat_touch_get_point(seat->seat, wlr_icon->drag->touch_id);
		if (!point) {
			return;
		}
		drag_icon->lx = seat->touch_lx;
		drag_icon->ly = seat->touch_ly;
		break;
	}

	wlr_scene_node_set_position(&drag_icon->scene_tree->node, drag_icon->lx, drag_icon->ly);
}

static void
handle_drag_icon_destroy(struct wl_listener *listener, void *data)
{
	struct cg_drag_icon *drag_icon = wl_container_of(listener, drag_icon, destroy);

	wl_list_remove(&drag_icon->link);
	wl_list_remove(&drag_icon->destroy.link);
	wlr_scene_node_destroy(&drag_icon->scene_tree->node);
	free(drag_icon);
}

static void
handle_request_start_drag(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat->seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(seat->seat, event->drag, event->serial);
		return;
	}

	struct wlr_touch_point *point;
	if (wlr_seat_validate_touch_grab_serial(seat->seat, event->origin, event->serial, &point)) {
		wlr_seat_start_touch_drag(seat->seat, event->drag, event->serial, point);
		return;
	}

	// TODO: tablet grabs
	wlr_log(WLR_DEBUG, "Ignoring start_drag request: could not validate pointer/touch serial %" PRIu32,
		event->serial);
	wlr_data_source_destroy(event->drag->source);
}

static void
handle_start_drag(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, start_drag);
	struct wlr_drag *wlr_drag = data;
	struct wlr_drag_icon *wlr_drag_icon = wlr_drag->icon;
	if (wlr_drag_icon == NULL) {
		return;
	}

	struct cg_drag_icon *drag_icon = calloc(1, sizeof(struct cg_drag_icon));
	if (!drag_icon) {
		return;
	}
	drag_icon->seat = seat;
	drag_icon->wlr_drag_icon = wlr_drag_icon;
	drag_icon->scene_tree = wlr_scene_subsurface_tree_create(&seat->server->scene->tree, wlr_drag_icon->surface);
	if (!drag_icon->scene_tree) {
		free(drag_icon);
		return;
	}

	drag_icon->destroy.notify = handle_drag_icon_destroy;
	wl_signal_add(&wlr_drag_icon->events.destroy, &drag_icon->destroy);

	wl_list_insert(&seat->drag_icons, &drag_icon->link);

	drag_icon_update_position(drag_icon);
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_seat *seat = wl_container_of(listener, seat, destroy);
	wl_list_remove(&seat->destroy.link);
	wl_list_remove(&seat->cursor_motion_relative.link);
	wl_list_remove(&seat->cursor_motion_absolute.link);
	wl_list_remove(&seat->cursor_button.link);
	wl_list_remove(&seat->cursor_axis.link);
	wl_list_remove(&seat->cursor_frame.link);
	wl_list_remove(&seat->touch_down.link);
	wl_list_remove(&seat->touch_up.link);
	wl_list_remove(&seat->touch_motion.link);
	wl_list_remove(&seat->touch_frame.link);
	wl_list_remove(&seat->tablet_tool_axis.link);
	wl_list_remove(&seat->tablet_tool_proximity.link);
	wl_list_remove(&seat->tablet_tool_tip.link);
	wl_list_remove(&seat->tablet_tool_button.link);
	wl_list_remove(&seat->request_set_cursor.link);
	wl_list_remove(&seat->request_set_selection.link);
	wl_list_remove(&seat->request_set_primary_selection.link);

	struct cg_keyboard_group *group, *group_tmp;
	wl_list_for_each_safe (group, group_tmp, &seat->keyboard_groups, link) {
		wlr_keyboard_group_destroy(group->wlr_group);
		free(group);
	}
	struct cg_pointer *pointer, *pointer_tmp;
	wl_list_for_each_safe (pointer, pointer_tmp, &seat->pointers, link) {
		handle_pointer_destroy(&pointer->destroy, NULL);
	}
	struct cg_touch *touch, *touch_tmp;
	wl_list_for_each_safe (touch, touch_tmp, &seat->touch, link) {
		handle_touch_destroy(&touch->destroy, NULL);
	}
	struct cg_tablet *tablet, *tablet_tmp;
	wl_list_for_each_safe (tablet, tablet_tmp, &seat->tablets, link) {
		handle_tablet_destroy(&tablet->destroy, NULL);
	}
	wl_list_remove(&seat->new_input.link);

	wlr_xcursor_manager_destroy(seat->xcursor_manager);
	if (seat->cursor) {
		wlr_cursor_destroy(seat->cursor);
	}
	free(seat);
}

#ifdef SG_TEST_TABLET
/* Test builds only (meson -Dtest-tablet=true; test/pen-gate.sh): a pen for a
 * headless compositor, which has no input devices. SG_TEST_TABLET_FIFO names
 * a FIFO of lines -- "in X Y", "move X Y", "down", "up", "button CODE 1|0"
 * (X, Y from 0 to 1), "pressure P" (0 to 1), "tilt X Y" (degrees), "out"
 * -- emitted as a tablet tool's events, through the same wlr_cursor path a
 * real pen's take; and a touch screen's: "tdown ID X Y", "tmove ID X Y",
 * "tup ID" (each with its frame). With SG_TEST_TABLET_LATE set, the tablet
 * is plugged in only at a "plug" line. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <wlr/interfaces/wlr_tablet_tool.h>
#include <wlr/interfaces/wlr_touch.h>

static const struct wlr_tablet_impl test_tablet_impl = {.name = "sg-test-pen"};
static const struct wlr_touch_impl test_touch_impl = {.name = "sg-test-touch"};
static struct wlr_touch test_touch;
static struct cg_seat *test_seat;
static struct wlr_tablet test_tablet;
static struct wlr_tablet_tool test_tool;
static char test_buf[4096];
static size_t test_len;
static double test_x = 0.5, test_y = 0.5;
static bool test_plugged;

static uint32_t
test_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
test_tablet_line(const char *line)
{
	double x, y;
	unsigned code, pressed;
	int id;
	if (sscanf(line, "pressure %lf", &x) == 1) {
		struct wlr_tablet_tool_axis_event ev = {.tablet = &test_tablet,
							.tool = &test_tool,
							.time_msec = test_now(),
							.updated_axes = WLR_TABLET_TOOL_AXIS_PRESSURE,
							.x = test_x,
							.y = test_y,
							.pressure = x};
		wl_signal_emit_mutable(&test_tablet.events.axis, &ev);
	} else if (sscanf(line, "tilt %lf %lf", &x, &y) == 2) {
		struct wlr_tablet_tool_axis_event ev = {.tablet = &test_tablet,
							.tool = &test_tool,
							.time_msec = test_now(),
							.updated_axes = WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y,
							.x = test_x,
							.y = test_y,
							.tilt_x = x,
							.tilt_y = y};
		wl_signal_emit_mutable(&test_tablet.events.axis, &ev);
	} else if (!strncmp(line, "out", 3)) {
		struct wlr_tablet_tool_proximity_event ev = {.tablet = &test_tablet,
							     .tool = &test_tool,
							     .time_msec = test_now(),
							     .x = test_x,
							     .y = test_y,
							     .state = WLR_TABLET_TOOL_PROXIMITY_OUT};
		wl_signal_emit_mutable(&test_tablet.events.proximity, &ev);
	} else if (sscanf(line, "tdown %d %lf %lf", &id, &x, &y) == 3) {
		/* where a touch there lands, as handle_touch_down maps it */
		double lx, ly;
		wlr_cursor_absolute_to_layout_coords(test_seat->cursor, &test_touch.base, x, y, &lx, &ly);
		wlr_log(WLR_ERROR, "sg-test: touch at %.0f %.0f", lx, ly);
		struct wlr_touch_down_event ev = {
			.touch = &test_touch, .time_msec = test_now(), .touch_id = id, .x = x, .y = y};
		wl_signal_emit_mutable(&test_touch.events.down, &ev);
		wl_signal_emit_mutable(&test_touch.events.frame, NULL);
	} else if (sscanf(line, "tmove %d %lf %lf", &id, &x, &y) == 3) {
		struct wlr_touch_motion_event ev = {
			.touch = &test_touch, .time_msec = test_now(), .touch_id = id, .x = x, .y = y};
		wl_signal_emit_mutable(&test_touch.events.motion, &ev);
		wl_signal_emit_mutable(&test_touch.events.frame, NULL);
	} else if (sscanf(line, "tup %d", &id) == 1) {
		struct wlr_touch_up_event ev = {.touch = &test_touch, .time_msec = test_now(), .touch_id = id};
		wl_signal_emit_mutable(&test_touch.events.up, &ev);
		wl_signal_emit_mutable(&test_touch.events.frame, NULL);
	} else if (!strncmp(line, "plug", 4)) {
		/* SG_TEST_TABLET_LATE: the tablet arrives now (a pen first seen
		 * after programs started) */
		if (!test_plugged) {
			test_plugged = true;
			test_seat->new_input.notify(&test_seat->new_input, &test_tablet.base);
		}
	} else if (sscanf(line, "in %lf %lf", &x, &y) == 2) {
		test_x = x, test_y = y;
		struct wlr_tablet_tool_proximity_event ev = {.tablet = &test_tablet,
							     .tool = &test_tool,
							     .time_msec = test_now(),
							     .x = x,
							     .y = y,
							     .state = WLR_TABLET_TOOL_PROXIMITY_IN};
		wl_signal_emit_mutable(&test_tablet.events.proximity, &ev);
		wlr_log(WLR_ERROR, "sg-test: pen at %.0f %.0f", test_seat->cursor->x, test_seat->cursor->y);
	} else if (sscanf(line, "move %lf %lf", &x, &y) == 2) {
		test_x = x, test_y = y;
		struct wlr_tablet_tool_axis_event ev = {.tablet = &test_tablet,
							.tool = &test_tool,
							.time_msec = test_now(),
							.updated_axes = WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y,
							.x = x,
							.y = y};
		wl_signal_emit_mutable(&test_tablet.events.axis, &ev);
	} else if (!strncmp(line, "down", 4) || !strncmp(line, "up", 2)) {
		struct wlr_tablet_tool_tip_event ev = {.tablet = &test_tablet,
						       .tool = &test_tool,
						       .time_msec = test_now(),
						       .x = test_x,
						       .y = test_y,
						       .state = line[0] == 'd' ? WLR_TABLET_TOOL_TIP_DOWN
									       : WLR_TABLET_TOOL_TIP_UP};
		wl_signal_emit_mutable(&test_tablet.events.tip, &ev);
	} else if (sscanf(line, "button %u %u", &code, &pressed) == 2) {
		struct wlr_tablet_tool_button_event ev = {.tablet = &test_tablet,
							  .tool = &test_tool,
							  .time_msec = test_now(),
							  .button = code,
							  .state = pressed ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED};
		wl_signal_emit_mutable(&test_tablet.events.button, &ev);
	}
}

static int
handle_test_tablet_fd(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	while ((n = read(fd, test_buf + test_len, sizeof(test_buf) - 1 - test_len)) > 0) {
		test_len += n;
		test_buf[test_len] = 0;
		char *nl;
		while ((nl = strchr(test_buf, '\n'))) {
			*nl = 0;
			test_tablet_line(test_buf);
			test_len -= nl + 1 - test_buf;
			memmove(test_buf, nl + 1, test_len + 1);
		}
		if (test_len == sizeof(test_buf) - 1) {
			test_len = 0;
		}
	}
	return 0;
}

static void
seat_test_tablet(struct cg_seat *seat)
{
	const char *path = getenv("SG_TEST_TABLET_FIFO");
	if (!path) {
		return;
	}
	/* read-write: no end of file when a writer closes */
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "Cannot open the test pen's FIFO %s: %s", path, strerror(errno));
		return;
	}
	test_seat = seat;
	wlr_tablet_init(&test_tablet, &test_tablet_impl, "sg-test-pen");
	test_tool.type = WLR_TABLET_TOOL_TYPE_PEN;
	test_tool.pressure = test_tool.tilt = true;
	wl_signal_init(&test_tool.events.destroy);
	if (!getenv("SG_TEST_TABLET_LATE")) {
		test_plugged = true;
		seat->new_input.notify(&seat->new_input, &test_tablet.base);
	}
	wlr_touch_init(&test_touch, &test_touch_impl, "sg-test-touch");
	seat->new_input.notify(&seat->new_input, &test_touch.base);
	wl_event_loop_add_fd(wl_display_get_event_loop(seat->server->wl_display), fd, WL_EVENT_READABLE,
			     handle_test_tablet_fd, seat);
}
#endif

struct cg_seat *
seat_create(struct cg_server *server, struct wlr_backend *backend)
{
	struct cg_seat *seat = calloc(1, sizeof(struct cg_seat));
	if (!seat) {
		wlr_log(WLR_ERROR, "Cannot allocate seat");
		return NULL;
	}

	seat->seat = wlr_seat_create(server->wl_display, "seat0");
	if (!seat->seat) {
		wlr_log(WLR_ERROR, "Cannot allocate seat0");
		free(seat);
		return NULL;
	}
	seat->server = server;
	seat->destroy.notify = handle_destroy;
	wl_signal_add(&seat->seat->events.destroy, &seat->destroy);

	seat->cursor = wlr_cursor_create();
	if (!seat->cursor) {
		wlr_log(WLR_ERROR, "Unable to create cursor");
		wl_list_remove(&seat->destroy.link);
		free(seat);
		return NULL;
	}
	wlr_cursor_attach_output_layout(seat->cursor, server->output_layout);

	if (!seat->xcursor_manager) {
		seat->xcursor_manager = wlr_xcursor_manager_create(NULL, XCURSOR_SIZE);
		seat->cursor_size = XCURSOR_SIZE;
		if (!seat->xcursor_manager) {
			wlr_log(WLR_ERROR, "Cannot create XCursor manager");
			wlr_cursor_destroy(seat->cursor);
			wl_list_remove(&seat->destroy.link);
			free(seat);
			return NULL;
		}
	}

	seat->cursor_motion_relative.notify = handle_cursor_motion_relative;
	wl_signal_add(&seat->cursor->events.motion, &seat->cursor_motion_relative);
	seat->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&seat->cursor->events.motion_absolute, &seat->cursor_motion_absolute);
	seat->cursor_button.notify = handle_cursor_button;
	wl_signal_add(&seat->cursor->events.button, &seat->cursor_button);
	seat->cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&seat->cursor->events.axis, &seat->cursor_axis);
	seat->cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&seat->cursor->events.frame, &seat->cursor_frame);

	seat->touch_down.notify = handle_touch_down;
	wl_signal_add(&seat->cursor->events.touch_down, &seat->touch_down);
	seat->touch_up.notify = handle_touch_up;
	wl_signal_add(&seat->cursor->events.touch_up, &seat->touch_up);
	seat->touch_motion.notify = handle_touch_motion;
	wl_signal_add(&seat->cursor->events.touch_motion, &seat->touch_motion);
	seat->touch_frame.notify = handle_touch_frame;
	wl_signal_add(&seat->cursor->events.touch_frame, &seat->touch_frame);

	seat->tablet_tool_axis.notify = handle_tablet_tool_axis;
	wl_signal_add(&seat->cursor->events.tablet_tool_axis, &seat->tablet_tool_axis);
	seat->tablet_tool_proximity.notify = handle_tablet_tool_proximity;
	wl_signal_add(&seat->cursor->events.tablet_tool_proximity, &seat->tablet_tool_proximity);
	seat->tablet_tool_tip.notify = handle_tablet_tool_tip;
	wl_signal_add(&seat->cursor->events.tablet_tool_tip, &seat->tablet_tool_tip);
	seat->tablet_tool_button.notify = handle_tablet_tool_button;
	wl_signal_add(&seat->cursor->events.tablet_tool_button, &seat->tablet_tool_button);

	seat->request_set_cursor.notify = handle_request_set_cursor;
	wl_signal_add(&seat->seat->events.request_set_cursor, &seat->request_set_cursor);
	seat->request_set_selection.notify = handle_request_set_selection;
	wl_signal_add(&seat->seat->events.request_set_selection, &seat->request_set_selection);
	seat->request_set_primary_selection.notify = handle_request_set_primary_selection;
	wl_signal_add(&seat->seat->events.request_set_primary_selection, &seat->request_set_primary_selection);

	wl_list_init(&seat->keyboards);
	wl_list_init(&seat->keyboard_groups);
	wl_list_init(&seat->pointers);
	wl_list_init(&seat->touch);
	wl_list_init(&seat->tablets);
	seat->tablet_manager = wlr_tablet_v2_create(server->wl_display);

	seat->new_input.notify = handle_new_input;
	wl_signal_add(&backend->events.new_input, &seat->new_input);

	server->new_virtual_keyboard.notify = handle_virtual_keyboard;
	server->new_virtual_pointer.notify = handle_virtual_pointer;

	wl_list_init(&seat->drag_icons);
	seat->request_start_drag.notify = handle_request_start_drag;
	wl_signal_add(&seat->seat->events.request_start_drag, &seat->request_start_drag);
	seat->start_drag.notify = handle_start_drag;
	wl_signal_add(&seat->seat->events.start_drag, &seat->start_drag);

#ifdef SG_TEST_TABLET
	seat_test_tablet(seat);
#endif
	return seat;
}

void
seat_destroy(struct cg_seat *seat)
{
	if (!seat) {
		return;
	}

	wl_list_remove(&seat->request_start_drag.link);
	wl_list_remove(&seat->start_drag.link);

	// Destroying the wlr seat will trigger the destroy handler on our seat,
	// which will in turn free it.
	wlr_seat_destroy(seat->seat);
}

struct cg_view *
seat_get_focus(struct cg_seat *seat)
{
	struct wlr_surface *prev_surface = seat->seat->keyboard_state.focused_surface;
	if (!prev_surface) {
		return NULL;
	}
	return view_from_wlr_surface(prev_surface);
}

void
seat_set_focus(struct cg_seat *seat, struct cg_view *view)
{
	struct cg_server *server = seat->server;
	struct wlr_seat *wlr_seat = seat->seat;
	struct cg_view *prev_view = seat_get_focus(seat);

	if (!view || prev_view == view) {
		return;
	}

	/* sg-compositor: while locked, keyboard focus -- and so every key
	 * event -- can only go to a privileged view. */
	if (!lock_view_allowed(&server->lock, view)) {
		return;
	}

#if CAGE_HAS_XWAYLAND
	if (view->type == CAGE_XWAYLAND_VIEW) {
		struct cg_xwayland_view *xwayland_view = xwayland_view_from_view(view);
		if (!wlr_xwayland_or_surface_wants_focus(xwayland_view->xwayland_surface)) {
			return;
		}
	}
#endif

	if (prev_view) {
		view_activate(prev_view, false);
	}

	/* Move the view to the front, but only if it isn't a
	   fullscreen view. */
	if (!view_is_primary(view) || view->elevated) {
		wl_list_remove(&view->link);
		wl_list_insert(&server->views, &view->link);
	}
	if (view->elevated) {
		elevated_view_focused(view);
	}

	view_activate(view, true);
	char *title = view_get_title(view);
	struct cg_output *output;
	wl_list_for_each (output, &server->outputs, link) {
		output_set_window_title(output, title);
	}
	free(title);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(wlr_seat);
	if (keyboard) {
		uint32_t keys[WLR_KEYBOARD_KEYS_CAP];
		size_t n = 0;
		for (size_t i = 0; i < keyboard->num_keycodes && n < WLR_KEYBOARD_KEYS_CAP; i++) {
#ifndef SG_MUTANT_ENTER_WITH_KEY
			if (seat->enter_skip_keycode && keyboard->keycodes[i] == seat->enter_skip_keycode) {
				continue;
			}
#endif
			keys[n++] = keyboard->keycodes[i];
		}
		wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface, keys, n, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(wlr_seat, view->wlr_surface, NULL, 0, NULL);
	}

	process_cursor_motion(seat, -1, 0, 0, 0, 0);
}

void
seat_center_cursor(struct cg_seat *seat)
{
	/* Place the cursor in the center of the output layout. */
	struct wlr_box layout_box;
	wlr_output_layout_get_box(seat->server->output_layout, NULL, &layout_box);
	wlr_cursor_warp(seat->cursor, NULL, layout_box.width / 2, layout_box.height / 2);
}
