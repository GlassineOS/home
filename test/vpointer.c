/* SPDX-License-Identifier: GPL-2.0-or-later
 * vpointer: one virtual pointer for the whole run (wlr-virtual-pointer), so a client sees
 * it arrive once and gets every click (wlrctl makes a new device per call, and a click
 * on a device the client has not bound yet is lost). Waits WAIT_MS for the clients to
 * bind it, then clicks the left button at each X,Y (logical px of the first output's
 * WIDTHxHEIGHT), PAUSE_MS apart.
 *   vpointer WIDTH HEIGHT WAIT_MS PAUSE_MS X,Y... */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *mgr;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg = { global, global_remove };

static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static void sleep_ms(int n)
{
	struct timespec t = { n / 1000, (n % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: vpointer WIDTH HEIGHT WAIT_MS PAUSE_MS X,Y...\n");
		return 2;
	}
	int w = atoi(argv[1]), h = atoi(argv[2]), wait = atoi(argv[3]), pause = atoi(argv[4]);
	struct wl_display *d = wl_display_connect(NULL);
	if (!d) {
		fprintf(stderr, "vpointer: no Wayland display\n");
		return 1;
	}
	struct wl_registry *r = wl_display_get_registry(d);
	wl_registry_add_listener(r, &reg, NULL);
	wl_display_roundtrip(d);
	if (!seat || !mgr) {
		fprintf(stderr, "vpointer: no seat or no zwlr_virtual_pointer_manager_v1\n");
		return 1;
	}
	struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
	wl_display_roundtrip(d);
	sleep_ms(wait);
	for (int i = 5; i < argc; i++) {
		int x, y;
		if (sscanf(argv[i], "%d,%d", &x, &y) != 2)
			continue;
		zwlr_virtual_pointer_v1_motion_absolute(p, now_ms(), (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
		zwlr_virtual_pointer_v1_frame(p);
		wl_display_roundtrip(d);
		sleep_ms(50);
		zwlr_virtual_pointer_v1_button(p, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
		zwlr_virtual_pointer_v1_frame(p);
		wl_display_roundtrip(d);
		sleep_ms(50);
		zwlr_virtual_pointer_v1_button(p, now_ms(), BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		zwlr_virtual_pointer_v1_frame(p);
		wl_display_roundtrip(d);
		sleep_ms(pause);
	}
	zwlr_virtual_pointer_v1_destroy(p);
	wl_display_roundtrip(d);
	wl_display_disconnect(d);
	return 0;
}
