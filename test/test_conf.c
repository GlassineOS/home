// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/conf.h"
#include "../src/pack.h"
#include "t.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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

	/* conf_read has no side effects: a broken file stays where it is */
	write_file("version = 1\nfavourites = \"x\"\n");
	size_t len;
	enum conf_state st;
	char *text = conf_read(path, &len, &st, &c, err, sizeof err);
	CHECK_INT(st, CONF_BROKEN);
	CHECK(text && !strcmp(text, "version = 1\nfavourites = \"x\"\n"));
	CHECK(access(path, F_OK) == 0);
	free(text);
	conf_free(&c);
	write_file("version = 3\n");
	free(conf_read(path, &len, &st, &c, err, sizeof err));
	CHECK_INT(st, CONF_NEWER);
	conf_free(&c);
	unlink(path);
	CHECK(conf_read(path, &len, &st, &c, err, sizeof err) == NULL);
	CHECK_INT(st, CONF_MISSING);
	write_file("version = 1\nfavourites = [\"a\", \"b\"]\n");
	text = conf_read(path, &len, &st, &c, err, sizeof err);
	CHECK_INT(st, CONF_OK);
	CHECK_INT(c.nfav, 2);
	free(text);

	/* the format Home writes parses back to the same layout */
	CHECK_INT(conf_add(&c, "home/app-category", 2, NULL), 0);
	text = conf_format(&c, &len, err, sizeof err);
	CHECK(text != NULL);
	struct home_conf e;
	CHECK(text && conf_parse(text, len, &e, err, sizeof err));
	CHECK(conf_favourites_equal(&c, &e));
	CHECK(conf_widgets_equal(&c, &e));
	e.w[0].size = 3;
	CHECK(!conf_widgets_equal(&c, &e));
	free(e.favourites[1]);
	e.nfav = 1;
	CHECK(!conf_favourites_equal(&c, &e));
	conf_take_favourites(&c, &e);
	CHECK_INT(c.nfav, 1);
	CHECK_INT(e.nfav, 0);
	conf_take_widgets(&c, &e);
	CHECK_INT(c.w[0].size, 3);
	CHECK_INT(e.n, 0);
	conf_free(&e);
	free(text);

	/* the lock: a second holder waits; conf_save waits for it too */
	int pfd[2];
	CHECK(pipe(pfd) == 0);
	pid_t kid = fork();
	if (kid == 0) {
		int fd = conf_lock(path, -1);
		char ok = fd >= 0 ? 'y' : 'n';
		if (write(pfd[1], &ok, 1) != 1)
			_exit(1);
		usleep(400 * 1000);
		conf_unlock(fd);
		_exit(0);
	}
	char ok = 0;
	CHECK(read(pfd[0], &ok, 1) == 1 && ok == 'y');
	char lockp[1100];
	snprintf(lockp, sizeof lockp, "%s.lock", path);
	CHECK(access(lockp, F_OK) == 0);
	CHECK(conf_lock(path, 50) < 0);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	CHECK(conf_save(path, &c, err, sizeof err));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	CHECK(waited >= 250);
	waitpid(kid, NULL, 0);
	int lk = conf_lock(path, 0);
	CHECK(lk >= 0);
	conf_unlock(lk);
	CHECK(access(lockp, F_OK) == 0); /* never removed */
	close(pfd[0]);
	close(pfd[1]);
	conf_free(&c);

	/* duplicate instance ids are refused */
	write_file("version = 1\n[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000000\"\nwidget = \"builtin/reading\"\nsize = \"wide\"\n"
		   "[[widget]]\nuuid = \"00000000-0000-4000-8000-000000000000\"\nwidget = \"builtin/reading\"\nsize = \"half\"\n");
	CHECK(!conf_load(path, &c, err, sizeof err));
	conf_free(&c);
	return t_done("conf");
}
