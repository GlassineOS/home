// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/places.h"
#include "t.h"

#include <stdlib.h>

static const char *path = "out/places-test/places";

static const char *load(const char *content, struct places *p)
{
	if (content) {
		FILE *f = fopen(path, "w");
		fputs(content, f);
		fclose(f);
	} else {
		remove(path);
	}
	places_load(p, path, "/u");
	const char *b = places_get(p, "books", 5);
	return b ? b : p->why;
}

int main(void)
{
	if (system("rm -rf out/places-test && mkdir -p out/places-test"))
		return 2;
	struct places p;
	/* no file: the default */
	CHECK_STR(load(NULL, &p), "/u/Books");
	places_load(&p, NULL, "/u");
	CHECK_STR(places_get(&p, "books", 5), "/u/Books");
	/* set */
	CHECK_STR(load("books = \"/media/library\"\n", &p), "/media/library");
	CHECK_STR(load("books = \"~/Library/\"\n", &p), "/u/Library");
	/* other names are ignored; the default stays */
	CHECK_STR(load("music = \"~/Music\"\n", &p), "/u/Books");
	CHECK(places_get(&p, "music", 5) == NULL);
	CHECK(!place_known("music", 5) && place_known("books", 5));
	/* unset or not allowed: the place is unset, never something wider */
	CHECK_STR(load("books = \"\"\n", &p), "not set");
	CHECK_STR(load("books = \"Library\"\n", &p), "\"Library\" is not absolute or ~/");
	CHECK_STR(load("books = \"/\"\n", &p), "\"/\" is not an allowed folder");
	CHECK_STR(load("books = \"/proc/self\"\n", &p), "\"/proc/self\" is not an allowed folder");
	CHECK_STR(load("books = \"/sys/class\"\n", &p), "\"/sys/class\" is not an allowed folder");
	CHECK_STR(load("books = \"~/a/../b\"\n", &p), "\"~/a/../b\" is not an allowed folder");
	CHECK_STR(load("books = 3\n", &p), "books is not a path");
	CHECK_STR(load("books = \n", &p), "the places file can't be read");

	char out[256];
	CHECK(places_path("/u", out, sizeof out));
	CHECK_STR(out, "/u/.config/glassine/places");
	return t_done("places");
}
