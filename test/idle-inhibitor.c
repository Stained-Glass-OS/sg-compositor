/* A Wayland client that holds an idle inhibitor (idle-inhibit-unstable-v1)
 * on a surface of its own until it is killed -- what a video player does
 * while it plays. The power gate uses it to check that the compositor's idle
 * notifications (swayidle) are held off while an inhibitor exists.
 *
 * Built by test/power-gate.sh with wayland-scanner's client code.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "idle-inhibit-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct zwp_idle_inhibit_manager_v1 *manager;

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    if (!strcmp(iface, wl_compositor_interface.name))
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 1);
    else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name))
        manager = wl_registry_bind(reg, name, &zwp_idle_inhibit_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener listener = { global, global_remove };

int main(void)
{
    struct wl_display *dpy = wl_display_connect(NULL);
    struct wl_surface *surface;

    if (!dpy) { fprintf(stderr, "no Wayland display\n"); return 1; }
    wl_registry_add_listener(wl_display_get_registry(dpy), &listener, NULL);
    wl_display_roundtrip(dpy);
    if (!compositor || !manager) { fprintf(stderr, "no idle inhibit manager\n"); return 1; }
    surface = wl_compositor_create_surface(compositor);
    zwp_idle_inhibit_manager_v1_create_inhibitor(manager, surface);
    wl_display_roundtrip(dpy);
    printf("inhibiting\n");
    fflush(stdout);
    while (wl_display_dispatch(dpy) != -1) ;
    return 0;
}
