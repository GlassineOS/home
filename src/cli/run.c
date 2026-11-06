// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * glassine-home-run: runs one widget once through the real sandbox and prints its
 * canonical description. For widget authors and tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../manifest.h"
#include "../sandbox.h"

static void usage(void)
{
	fprintf(stderr,
		"usage: glassine-home-run [--edit] [--props JSON] [--apps JSON] [--now SECONDS] [-v] WIDGETDIR\n"
		"  Runs WIDGETDIR/widget.lua (or widget.edit.lua with --edit) once in the sandbox and\n"
		"  prints the canonical description. --props overrides settings (a JSON object);\n"
		"  -v also lists the files the run asked for and its print() lines.\n"
		"  Exit status: 0 ok, 1 the run failed, 2 usage or manifest error.\n");
	exit(2);
}

int main(int argc, char **argv)
{
	/* the zygote first, before anything else is allocated */
	struct zygote z;
	if (!zygote_start(&z)) {
		perror("glassine-home-run: zygote");
		return 2;
	}
	struct run_opts o = { .now = (long long)time(NULL) };
	const char *props_json = NULL, *apps_json = NULL, *dir = NULL;
	bool verbose = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--edit"))
			o.edit = true;
		else if (!strcmp(argv[i], "-v"))
			verbose = true;
		else if (!strcmp(argv[i], "--props") && i + 1 < argc)
			props_json = argv[++i];
		else if (!strcmp(argv[i], "--apps") && i + 1 < argc)
			apps_json = argv[++i];
		else if (!strcmp(argv[i], "--now") && i + 1 < argc)
			o.now = strtoll(argv[++i], NULL, 10);
		else if (argv[i][0] == '-' || dir)
			usage();
		else
			dir = argv[i];
	}
	if (!dir)
		usage();

	char err[512];
	struct manifest m;
	if (!manifest_load(dir, &m, err, sizeof err)) {
		fprintf(stderr, "%s: %s\n", dir, err);
		return 2;
	}
	if (o.edit && !m.has_edit) {
		fprintf(stderr, "%s: no widget.edit.lua\n", dir);
		return 2;
	}
	struct value *saved = NULL, *apps = NULL;
	if (props_json && !(saved = v_from_json(props_json, strlen(props_json), 8, err, sizeof err))) {
		fprintf(stderr, "--props: %s\n", err);
		return 2;
	}
	if (saved && saved->t != V_MAP) {
		fprintf(stderr, "--props: must be a JSON object\n");
		return 2;
	}
	if (apps_json && !(apps = v_from_json(apps_json, strlen(apps_json), 8, err, sizeof err))) {
		fprintf(stderr, "--apps: %s\n", err);
		return 2;
	}
	o.apps = apps;
	struct value *props = props_effective(&m, saved);

	struct run_result r;
	run_widget(&z, dir, &m, props, &o, &r);
	if (verbose) {
		for (size_t i = 0; i < r.nfiles; i++)
			fprintf(stderr, "read: %s\n", r.files[i]);
		for (size_t i = 0; i < r.nlog; i++)
			fprintf(stderr, "print: %s\n", r.log[i]);
	}
	int rc = 0;
	if (r.status == RUN_OK) {
		printf("%s\n", r.json.p);
	} else {
		fprintf(stderr, "%s: %s: %s\n", dir, run_status_name(r.status), r.msg);
		rc = 1;
	}
	run_result_free(&r);
	v_free(props);
	v_free(saved);
	v_free(apps);
	manifest_free(&m);
	zygote_stop(&z);
	return rc;
}
