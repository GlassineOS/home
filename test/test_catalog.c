// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/catalog.h"
#include "../src/conf.h"
#include "t.h"

#include <stdlib.h>
#include <unistd.h>

int main(void)
{
	if (system("rm -rf out/catalog-test && mkdir -p out/catalog-test/user out/catalog-test/system && "
		   "cd out/catalog-test && cp -R ../../widgets builtin && "
		   "mkdir user/clock user/nolua user/bad user/Bad-Name user/.hidden && "
		   "printf 'name = \"Clock\"\\nicon = \"schedule\"\\nsizes = [\"half\"]\\nrefresh = { every = \"15m\" }\\n' > user/clock/widget.toml && "
		   "touch user/clock/widget.lua && cp user/clock/widget.toml user/nolua/ && "
		   "printf 'name = \"Bad\"\\n' > user/bad/widget.toml && touch user/bad/widget.lua && "
		   "ln -s clock user/linked && touch user/afile"))
		return 2;
	struct catalog c;
	catalog_scan(&c, "out/catalog-test/builtin", "out/catalog-test/system", "out/catalog-test/user");
	/* sorted by ref; the link, the plain file and the hidden folder are not widgets */
	CHECK_INT(c.n, 5);
	const char *want[] = { "builtin/reading", "user/Bad-Name", "user/bad", "user/clock", "user/nolua" };
	for (size_t i = 0; i < 5 && i < c.n; i++)
		CHECK_STR(c.e[i].ref, want[i]);

	const struct catalog_entry *e = catalog_find(&c, "builtin/reading");
	CHECK(e && e->ok);
	if (e) {
		CHECK_STR(e->m.name, "Continue reading");
		CHECK(e->m.on_change);
		CHECK(conf_widget_ref_valid(e->ref));
	}
	e = catalog_find(&c, "user/clock");
	CHECK(e && e->ok && e->m.every_s == 900);
	/* broken folders stay listed with their error */
	e = catalog_find(&c, "user/nolua");
	CHECK(e && !e->ok);
	if (e)
		CHECK_STR(e->err, "widget.lua: missing");
	e = catalog_find(&c, "user/bad");
	CHECK(e && !e->ok);
	if (e)
		CHECK_STR(e->err, "icon: missing");
	e = catalog_find(&c, "user/Bad-Name");
	CHECK(e && !e->ok);
	CHECK(!catalog_find(&c, "user/linked"));
	CHECK(!catalog_find(&c, "user/missing"));
	CHECK_STR(c.roots[2], "out/catalog-test/user");
	catalog_free(&c);

	/* a new folder appears after a rescan */
	if (system("cd out/catalog-test/user && mkdir later && cp clock/widget.toml clock/widget.lua later/"))
		return 2;
	catalog_scan(&c, NULL, NULL, "out/catalog-test/user");
	CHECK(catalog_find(&c, "user/later") != NULL);
	CHECK(c.roots[0] == NULL);
	catalog_free(&c);
	return t_done("catalog");
}
