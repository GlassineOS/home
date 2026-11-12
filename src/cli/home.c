// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * glassine-home: the Home screen (spec 001). A Wayland app (app_id glassine-home) that
 * the session shows over the current screen.
 *
 * Environment (defaults in brackets):
 *   GLASSINE_HOME_LAUNCH   the command that opens an app [glassine-shell-wl launch]; Home adds
 *                          the desktop id, new|here and, for a widget's file, the file
 *   GLASSINE_HOME_HIDE     the command that hides Home [glassine-swayd home hide]
 *   GLASSINE_HOME_WIDGETS  the built-in widgets [/usr/share/glassine-home/widgets]
 *   GLASSINE_HOME_NATIVE   the settings scripts of Home's own app widgets [/usr/share/glassine-home/native]
 *   GLASSINE_HOME_VOICE    white | editorial | stationery [~/.config/glassine-shell/theme]
 *   GLASSINE_HOME_SCALE    device px per design px [the panel's short side / 702]
 *   GLASSINE_HOME_APPS     the application directories, colon-separated (tests) [the XDG ones]
 *
 * Exits 1 when the widget helper (the zygote) stops, an out-of-memory kill say: it
 * can't be forked again once Home has threads, so the session starts Home again.
 */
#include <stdio.h>
#include <string.h>

#include "../sandbox.h"

int home_wayland_main(struct zygote *z);

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("usage: glassine-home\nThe Home screen; see the comment at the top of src/cli/home.c for the environment.\n");
			return 0;
		}
	/* the zygote first, before any thread or large allocation (sandbox.h) */
	struct zygote z;
	if (!zygote_start(&z)) {
		perror("glassine-home: zygote");
		return 1;
	}
	int r = home_wayland_main(&z);
	zygote_stop(&z);
	return r;
}
