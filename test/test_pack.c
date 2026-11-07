// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/pack.h"
#include "t.h"

#define H SIZE_HALF
#define W SIZE_WIDE
#define N(a) (sizeof(a) / sizeof *(a))

static struct pack_pos pos[32];

/* "page.lane.row.col" for widget i */
static const char *at(int i)
{
	static char s[32];
	snprintf(s, sizeof s, "%d.%d.%d.%d", pos[i].page, pos[i].lane, pos[i].row, pos[i].col);
	return s;
}

static void portrait(void)
{
	enum orient P = ORIENT_PORTRAIT;
	CHECK_INT(pack(P, NULL, 0, pos), 0);

	/* a half fills the free half beside an earlier half */
	int a[] = { H, W, H };
	CHECK_INT(pack(P, a, N(a), pos), 1);
	CHECK_STR(at(0), "0.0.0.0");
	CHECK_STR(at(1), "0.0.1.0");
	CHECK_STR(at(2), "0.0.0.2");

	/* five wides fill a page, the sixth starts page 2 */
	int b[] = { W, W, W, W, W, W };
	CHECK_INT(pack(P, b, N(b), pos), 2);
	CHECK_STR(at(4), "0.0.4.0");
	CHECK_STR(at(5), "1.0.0.0");

	/* an app widget of 5 rows doesn't fit under a wide: it spills to the next page */
	int c[] = { W, 5 };
	CHECK_INT(pack(P, c, N(c), pos), 2);
	CHECK_STR(at(1), "1.0.0.0");
	CHECK_INT(pos[1].rows, 5);

	/* a half after a full page starts a new page; it never goes back to an earlier page */
	int d[] = { H, 5, H };
	CHECK_INT(pack(P, d, N(d), pos), 3);
	CHECK_STR(at(1), "1.0.0.0");
	CHECK_STR(at(2), "2.0.0.0");

	/* backfill above a tall widget, then continue below it */
	int e[] = { H, 3, H, H };
	CHECK_INT(pack(P, e, N(e), pos), 1);
	CHECK_STR(at(1), "0.0.1.0");
	CHECK_STR(at(2), "0.0.0.2");
	CHECK_STR(at(3), "0.0.4.0");

	/* resizing a widget to a taller size pushes the rest on: closing pages follows from packing */
	int f1[] = { W, W, W, 1, W };
	CHECK_INT(pack(P, f1, N(f1), pos), 1);
	int f2[] = { W, W, W, 3, W };
	CHECK_INT(pack(P, f2, N(f2), pos), 2);
	CHECK_STR(at(3), "1.0.0.0");
	CHECK_STR(at(4), "1.0.3.0");
	/* and removing the tall one closes the second page */
	int f3[] = { W, W, W, W };
	CHECK_INT(pack(P, f3, N(f3), pos), 1);

	/* adding goes to the end: the last page, or a new page when it is full */
	int g[] = { W, W, W, W, H, H, H };
	CHECK_INT(pack(P, g, N(g), pos), 2);
	CHECK_STR(at(5), "0.0.4.2");
	CHECK_STR(at(6), "1.0.0.0");
}

static void landscape(void)
{
	enum orient L = ORIENT_LANDSCAPE;
	struct grid g = pack_grid(L);
	CHECK_INT(g.lanes, 2);
	CHECK_INT(g.rows, 3);

	int a[] = { 3, 3 };
	CHECK_INT(pack(L, a, N(a), pos), 1);
	CHECK_STR(at(0), "0.0.0.0");
	CHECK_STR(at(1), "0.1.0.0");

	/* five rows are capped at three in landscape */
	int b[] = { 5 };
	pack(L, b, 1, pos);
	CHECK_INT(pos[0].rows, 3);

	/* a tall widget that doesn't fit the lane goes to the next lane; a half backfills */
	int c[] = { H, 3, H };
	CHECK_INT(pack(L, c, N(c), pos), 1);
	CHECK_STR(at(1), "0.1.0.0");
	CHECK_STR(at(2), "0.0.0.2");

	/* full-width widgets never move back into gaps: the wide goes to a new page */
	int d[] = { H, 3, W };
	CHECK_INT(pack(L, d, N(d), pos), 2);
	CHECK_STR(at(2), "1.0.0.0");
	/* but halves do fill the gap in the first lane */
	int d2[] = { H, 3, H, H, H };
	CHECK_INT(pack(L, d2, N(d2), pos), 1);
	CHECK_STR(at(3), "0.0.1.0");
	CHECK_STR(at(4), "0.0.1.2");

	/* six wides fill both lanes; the seventh starts page 2 */
	int e[] = { W, W, W, W, W, W, W };
	CHECK_INT(pack(L, e, N(e), pos), 2);
	CHECK_STR(at(2), "0.0.2.0");
	CHECK_STR(at(3), "0.1.0.0");
	CHECK_STR(at(6), "1.0.0.0");

	/* the same list packs differently by orientation */
	int f[] = { 5, W };
	CHECK_INT(pack(ORIENT_PORTRAIT, f, N(f), pos), 2);
	CHECK_INT(pack(L, f, N(f), pos), 1);
	CHECK_STR(at(1), "0.1.0.0");
}

static void arrange(void)
{
	/* swap allowed only where every widget keeps its page */
	int a[] = { H, H, W };
	CHECK(pack_can_swap(ORIENT_PORTRAIT, a, N(a), 1, 2));
	int b[] = { 2, 2, W, W };
	CHECK(!pack_can_swap(ORIENT_LANDSCAPE, b, N(b), 1, 3));
	CHECK(pack_can_swap(ORIENT_LANDSCAPE, b, N(b), 0, 1));
	CHECK(!pack_can_swap(ORIENT_LANDSCAPE, b, N(b), 1, 1));
	CHECK(!pack_can_swap(ORIENT_LANDSCAPE, b, N(b), 1, 9));

	/* swapping pages exchanges their widget runs */
	int c[] = { W, W, W, W, W, H, H };
	size_t order[7];
	CHECK(pack_swap_pages(ORIENT_PORTRAIT, c, N(c), 0, 1, order));
	size_t want[] = { 5, 6, 0, 1, 2, 3, 4 };
	for (size_t i = 0; i < N(want); i++)
		CHECK_INT(order[i], want[i]);
	CHECK(!pack_swap_pages(ORIENT_PORTRAIT, c, N(c), 0, 2, order));

	size_t first, count;
	pack(ORIENT_PORTRAIT, c, N(c), pos);
	pack_page_range(pos, N(c), 1, &first, &count);
	CHECK_INT(first, 5);
	CHECK_INT(count, 2);

	CHECK(pack_size_valid(5) && !pack_size_valid(4) && !pack_size_valid(0) && pack_size_valid(H));
}

int main(void)
{
	portrait();
	landscape();
	arrange();
	return t_done("pack");
}
