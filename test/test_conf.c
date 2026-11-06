// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/conf.h"
#include "../src/pack.h"
#include "t.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char path[1024];

static void write_file(const char *s)
{
	FILE *f = fopen(path, "w");
	fputs(s, f);
	fclose(f);
}

static char *read_file(const char *p)
{
	static char b[8192];
	FILE *f = fopen(p, "r");
	if (!f)
		return NULL;
	size_t n = fread(b, 1, sizeof b - 1, f);
	fclose(f);
	b[n] = 0;
	return b;
}

int main(void)
{
	char cwd[512], err[256];
	if (!getcwd(cwd, sizeof cwd))
		return 2;
	snprintf(path, sizeof path, "%s/out/conf-test/glassine/home.conf", cwd);
	system("rm -rf out/conf-test && mkdir -p out/conf-test");

	struct home_conf c;
	/* no file yet: an empty layout */
	CHECK(conf_load(path, &c, err, sizeof err));
	CHECK_INT(c.n, 0);

	struct value *p = v_map();
	v_map_set(p, "category", v_str("Games \"retro\""));
	v_map_set(p, "scale", v_float(1.5));
	CHECK_INT(conf_add(&c, "builtin/reading", SIZE_WIDE, NULL), 0);
	CHECK_INT(conf_add(&c, "home/app-category", 2, p), 1);
	CHECK_INT(conf_add(&c, "user/clock", SIZE_HALF, NULL), 2);
	CHECK_INT(conf_add(&c, "elsewhere/x", SIZE_HALF, NULL), -1);
	CHECK_INT(conf_add(&c, "user/../x", SIZE_HALF, NULL), -1);
	CHECK_INT(conf_add(&c, "user/x", 4, NULL), -1);
	CHECK(strcmp(c.w[0].uuid, c.w[1].uuid) != 0);
	CHECK(c.w[0].uuid[14] == '4');
	c.favourites = malloc(sizeof *c.favourites);
	c.favourites[0] = strdup("org.example.Reader");
	c.nfav = 1;
	v_free(p);

	/* written atomically (the directory is created), read back the same */
	CHECK(conf_save(path, &c, err, sizeof err));
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	CHECK(access(tmp, F_OK) != 0);
	struct home_conf d;
	CHECK(conf_load(path, &d, err, sizeof err));
	CHECK_INT(d.n, 3);
	for (size_t i = 0; i < 3 && i < d.n; i++) {
		CHECK_STR(d.w[i].uuid, c.w[i].uuid);
		CHECK_STR(d.w[i].widget, c.w[i].widget);
		CHECK_INT(d.w[i].size, c.w[i].size);
		CHECK(v_equal(d.w[i].props, c.w[i].props));
	}
	CHECK_INT(d.nfav, 1);
	/* the same layout writes the same bytes */
	char first[8192];
	snprintf(first, sizeof first, "%s", read_file(path));
	CHECK(conf_save(path, &d, err, sizeof err));
	CHECK_STR(read_file(path), first);
	conf_free(&d);

	/* a fresh home without ~/.config */
	char fresh[1100];
	snprintf(fresh, sizeof fresh, "%s/out/conf-test/fresh/.config/glassine/home.conf", cwd);
	system("mkdir -p out/conf-test/fresh");
	CHECK(conf_save(fresh, &c, err, sizeof err));

	/* arranging: move, remove, page swap by permutation */
	CHECK(conf_move(&c, 2, 0));
	CHECK_STR(c.w[0].widget, "user/clock");
	CHECK_STR(c.w[1].widget, "builtin/reading");
	size_t order[] = { 2, 0, 1 };
	CHECK(conf_reorder(&c, order));
	CHECK_STR(c.w[0].widget, "home/app-category");
	CHECK(conf_remove(&c, 0));
	CHECK_INT(c.n, 2);
	CHECK_INT(conf_find(&c, c.w[1].uuid), 1);
	conf_free(&c);

	/* a broken file is kept aside, Home starts empty */
	write_file("version = 1\n[[widget]]\nuuid = \"nope\"\n");
	CHECK(!conf_load(path, &c, err, sizeof err));
	CHECK(strstr(err, "kept as") != NULL);
	CHECK_INT(c.n, 0);
	char broken[1100];
	snprintf(broken, sizeof broken, "%s.broken", path);
	CHECK(access(broken, F_OK) == 0);
	CHECK(access(path, F_OK) != 0);

	/* a newer version is left alone and never overwritten */
	write_file("version = 2\nwhatever = true\n");
	CHECK(!conf_load(path, &c, err, sizeof err));
	CHECK(c.read_only);
	CHECK(!conf_save(path, &c, err, sizeof err));
	CHECK_STR(read_file(path), "version = 2\nwhatever = true\n");
	conf_free(&c);

	/* duplicate instance ids are refused */
	write_file("version = 1\n[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000000\"\nwidget = \"builtin/reading\"\nsize = \"wide\"\n"
		   "[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000000\"\nwidget = \"builtin/reading\"\nsize = \"half\"\n");
	CHECK(!conf_load(path, &c, err, sizeof err));
	conf_free(&c);
	return t_done("conf");
}
