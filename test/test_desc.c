// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/desc.h"
#include "t.h"

static char err[256];

/* normalize JSON input, return canonical JSON or "ERR: message" */
static const char *norm(const char *json, bool edit)
{
	static struct buf out;
	buf_free(&out);
	struct value *in = v_from_json(json, strlen(json), 16, err, sizeof err);
	if (!in)
		return "BAD TEST INPUT";
	struct value *n = desc_normalize(in, edit, NULL, NULL, err, sizeof err);
	v_free(in);
	if (!n) {
		buf_adds(&out, "ERR: ");
		buf_adds(&out, err);
		return out.p;
	}
	v_to_json(n, &out);
	v_free(n);
	return out.p;
}

static bool only_home(void *ctx, const char *path)
{
	return !strncmp(path, "/data/", 6);
}

int main(void)
{
	CHECK_STR(norm("[{\"t\":\"title\",\"text\":\"A\\nB\"},{\"t\":\"progress\",\"value\":0.4567}]", false),
		  "[{\"t\":\"title\",\"text\":\"A B\"},{\"permille\":457,\"t\":\"progress\"}]");
	CHECK_STR(norm("[{\"t\":\"progress\",\"value\":7}]", false), "[{\"permille\":1000,\"t\":\"progress\"}]");
	CHECK_STR(norm("[{\"t\":\"title\",\"text\":\"a\"},{\"t\":\"title\",\"text\":\"b\"}]", false), "ERR: at most one ui.title");
	CHECK_STR(norm("{}", false), "ERR: render must return a list of ui primitives");
	CHECK_STR(norm("[{\"t\":\"blink\"}]", false), "ERR: render returned something that is not a ui primitive");
	CHECK_STR(norm("[{\"t\":\"text\",\"text\":\"x\",\"color\":\"red\"}]", false), "ERR: ui.text: unknown field \"color\"");

	/* lists: more than four rows are cut */
	CHECK_STR(norm("[{\"t\":\"list\",\"rows\":[{\"label\":\"1\"},{\"label\":\"2\"},{\"label\":\"3\",\"meta\":\"m\"},"
		       "{\"label\":\"4\"},{\"label\":\"5\"}]}]", false),
		  "[{\"rows\":[{\"label\":\"1\"},{\"label\":\"2\"},{\"label\":\"3\",\"meta\":\"m\"},{\"label\":\"4\"}],\"t\":\"list\"}]");

	/* buttons and actions */
	const char *b = "{\"t\":\"button\",\"label\":\"Go\",\"icon\":\"play_arrow\",\"action\":{\"a\":\"open\",\"app\":\"koreader\",\"file\":\"/b.epub\"}}";
	char three[1024];
	snprintf(three, sizeof three, "[%s,%s,%s]", b, b, b);
	CHECK_STR(norm(three, false), "ERR: at most two ui.button");
	CHECK_STR(norm("[{\"t\":\"button\",\"label\":\"x\",\"action\":{\"a\":\"exec\",\"cmd\":\"rm\"}}]", false),
		  "ERR: an action must come from ui.open, ui.settings or ui.set");
	CHECK_STR(norm("[{\"t\":\"button\",\"label\":\"x\",\"action\":{\"a\":\"open\",\"app\":\"a b\"}}]", false),
		  "ERR: ui.open: the app must be an application id");
	CHECK_STR(norm("[{\"t\":\"button\",\"label\":\"x\",\"action\":{\"a\":\"open\",\"app\":\"a\",\"file\":\"rel\"}}]", false),
		  "ERR: ui.open: the file must be an absolute path");
	CHECK_STR(norm("[{\"t\":\"button\",\"label\":\"x\",\"action\":{\"a\":\"settings\",\"page\":\"e-ink\"}}]", false),
		  "[{\"action\":{\"a\":\"settings\",\"page\":\"e-ink\"},\"label\":\"x\",\"t\":\"button\"}]");

	/* ui.set and selected only in settings scripts */
	const char *set = "[{\"t\":\"list\",\"rows\":[{\"label\":\"Office\",\"selected\":true,\"action\":{\"a\":\"set\",\"props\":{\"category\":\"Office\"}}}]}]";
	CHECK_STR(norm(set, false), "ERR: selected works only in widget.edit.lua");
	CHECK_STR(norm(set, true),
		  "[{\"rows\":[{\"action\":{\"a\":\"set\",\"props\":{\"category\":\"Office\"}},\"label\":\"Office\",\"selected\":true}],\"t\":\"list\"}]");
	CHECK_STR(norm("[{\"t\":\"button\",\"label\":\"x\",\"action\":{\"a\":\"set\",\"props\":{\"k\":1}}}]", false),
		  "ERR: ui.set works only in widget.edit.lua");

	/* image slots */
	CHECK_STR(norm("[{\"t\":\"image\",\"src\":\"cover:/b.epub\",\"w\":64,\"h\":96,\"side\":\"left\",\"alt\":\"Cover\"}]", false),
		  "[{\"alt\":\"Cover\",\"fit\":\"cover\",\"h\":96,\"side\":\"left\",\"src\":\"cover:/b.epub\",\"t\":\"image\",\"w\":64}]");
	CHECK_STR(norm("[{\"t\":\"image\",\"src\":\"https://example.org/x.png\",\"h\":96,\"alt\":\"x\"}]", false),
		  "ERR: ui.image: src must be file:PATH, cover:PATH or icon:APP (no network sources)");
	CHECK_STR(norm("[{\"t\":\"image\",\"src\":\"icon:koreader\",\"h\":96,\"side\":\"left\",\"alt\":\"x\"}]", false),
		  "ERR: ui.image: an image beside the text needs w");
	CHECK_STR(norm("[{\"t\":\"image\",\"src\":\"icon:koreader\",\"h\":0.5,\"alt\":\"x\"}]", false),
		  "ERR: ui.image: h must be a whole number of pixels from 1 to 2000");
	CHECK_STR(norm("[{\"t\":\"image\",\"src\":\"icon:koreader\",\"h\":10,\"fit\":\"fill\",\"alt\":\"x\"}]", false),
		  "ERR: ui.image: fit must be \"cover\" or \"contain\"");
	CHECK_STR(norm("[{\"t\":\"image\",\"src\":\"icon:koreader\",\"h\":10}]", false), "ERR: ui.image: alt must be a string");
	const char *img = "{\"t\":\"image\",\"src\":\"icon:a\",\"h\":10,\"alt\":\"x\"}";
	snprintf(three, sizeof three, "[%s,%s,%s]", img, img, img);
	CHECK_STR(norm(three, false), "ERR: at most two ui.image");

	/* cover: a clean absolute path, nothing in a system area */
	static const char *const bad_covers[] = { "/proc/self/environ", "/dev/zero", "/sys/x", "/run/x",
		"/b/../etc/passwd", "/b/./c.epub", "/b//c.epub", "/b/c.epub/", "/proc" };
	for (size_t i = 0; i < sizeof bad_covers / sizeof *bad_covers; i++) {
		char j[256];
		snprintf(j, sizeof j, "[{\"t\":\"image\",\"src\":\"cover:%s\",\"h\":10,\"alt\":\"x\"}]", bad_covers[i]);
		CHECK_STR(norm(j, false), "ERR: ui.image: cover: needs an absolute path without '.' or '..' parts, "
					  "outside /proc, /sys, /dev and /run");
	}
	CHECK_STR(norm("[{\"t\":\"image\",\"src\":\"cover:/procedures/a b.epub\",\"h\":10,\"alt\":\"x\"}]", false),
		  "[{\"alt\":\"x\",\"fit\":\"cover\",\"h\":10,\"src\":\"cover:/procedures/a b.epub\",\"t\":\"image\"}]");

	/* file: sources must be in the reads */
	const char *fj = "[{\"t\":\"image\",\"src\":\"file:/etc/x.png\",\"h\":10,\"alt\":\"x\"}]";
	struct value *in = v_from_json(fj, strlen(fj), 8, err, sizeof err);
	CHECK(in);
	struct value *n = desc_normalize(in, false, only_home, NULL, err, sizeof err);
	CHECK(!n);
	CHECK_STR(err, "ui.image: /etc/x.png is not in the widget's reads");
	v_free(in);
	/* a message with a long path is cut at a character boundary, so it stays valid UTF-8 */
	char lj[1024] = "[{\"t\":\"image\",\"src\":\"file:/xy/";
	for (int i = 0; i < 300; i++)
		strcat(lj, "\xc3\xa9");
	strcat(lj, "\",\"h\":10,\"alt\":\"x\"}]");
	in = v_from_json(lj, strlen(lj), 8, err, sizeof err);
	CHECK(in);
	n = desc_normalize(in, false, only_home, NULL, err, sizeof err);
	CHECK(!n);
	CHECK(!strncmp(err, "ui.image: /xy/", 14) && strlen(err) >= 250 && utf8_valid(err, strlen(err)));
	v_free(in);

	/* strings: cut at a code point boundary; invalid UTF-8 refused */
	char longt[400] = "[{\"t\":\"title\",\"text\":\"";
	for (int i = 0; i < 70; i++)
		strcat(longt, "\xc3\xa9");
	strcat(longt, "\"}]");
	const char *r = norm(longt, false);
	CHECK(strlen(r) == strlen("[{\"t\":\"title\",\"text\":\"\"}]") + 120);
	CHECK_STR(norm("[{\"t\":\"meta\",\"text\":\"Read\",\"time\":1700000000.7}]", false),
		  "[{\"t\":\"meta\",\"text\":\"Read\",\"time\":1700000000}]");
	/* normalizing is idempotent: Home re-normalizes what the run normalized */
	const char *all = "[{\"t\":\"title\",\"text\":\"T\"},{\"t\":\"text\",\"text\":\"x\",\"soft\":true},"
		"{\"t\":\"progress\",\"value\":0.4231,\"label\":\"42 %\"},{\"t\":\"meta\",\"text\":\"m\",\"time\":5},"
		"{\"t\":\"list\",\"rows\":[{\"label\":\"a\",\"meta\":\"b\",\"selected\":true,\"action\":{\"a\":\"set\",\"props\":{\"k\":1}}}]},"
		"{\"t\":\"button\",\"label\":\"Go\",\"icon\":\"x\",\"action\":{\"a\":\"open\",\"app\":\"a\",\"file\":\"/f\"}},"
		"{\"t\":\"image\",\"src\":\"icon:a\",\"w\":3,\"h\":4,\"side\":\"right\",\"fit\":\"contain\",\"alt\":\"x\"}]";
	char once[2048];
	snprintf(once, sizeof once, "%s", norm(all, true));
	CHECK(strncmp(once, "ERR", 3) != 0);
	CHECK_STR(norm(once, true), once);
	return t_done("desc");
}
