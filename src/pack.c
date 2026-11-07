// SPDX-License-Identifier: GPL-2.0-or-later
#include "pack.h"

#include <stdlib.h>
#include <string.h>

#define MAX_LANES 2
#define MAX_ROWS 5
#define COLS 4

struct grid pack_grid(enum orient o)
{
	if (o == ORIENT_LANDSCAPE)
		return (struct grid){ .lanes = 2, .rows = 3, .cols = COLS };
	return (struct grid){ .lanes = 1, .rows = 5, .cols = COLS };
}

bool pack_size_valid(int size)
{
	return size == SIZE_HALF || size == SIZE_WIDE || size == 1 || size == 2 || size == 3 || size == 5;
}

struct page {
	/* used[lane][row]: bit 0 = left half, bit 1 = right half */
	unsigned char used[MAX_LANES][MAX_ROWS];
};

int pack(enum orient o, const int *sizes, size_t n, struct pack_pos *out)
{
	struct grid g = pack_grid(o);
	struct page pg;
	memset(&pg, 0, sizeof pg);
	int page = 0, lane = 0, row = 0; /* the flow position */
	bool any = false;

	for (size_t i = 0; i < n; i++) {
		int s = sizes[i];
		struct pack_pos *p = &out[i];
		if (s == SIZE_HALF) {
			bool placed = false;
			/* the lane the flow has reached, then the next ones; never an earlier lane */
			for (int l = lane; l < g.lanes && !placed; l++)
				for (int r = 0; r < g.rows && !placed; r++)
					for (int h = 0; h < 2 && !placed; h++)
						if (!(pg.used[l][r] & (1u << h))) {
							pg.used[l][r] |= (unsigned char)(1u << h);
							*p = (struct pack_pos){ page, l, r, h * 2, 2, 1 };
							placed = true;
							if (l > lane) {
								lane = l;
								row = r + 1;
							} else if (l == lane && r + 1 > row) {
								row = r + 1;
							}
						}
			if (!placed) {
				if (any)
					page++;
				memset(&pg, 0, sizeof pg);
				pg.used[0][0] = 1;
				*p = (struct pack_pos){ page, 0, 0, 0, 2, 1 };
				lane = 0;
				row = 1;
			}
			any = true;
			continue;
		}
		int rows = s == SIZE_WIDE ? 1 : s;
		if (rows < 1)
			rows = 1;
		if (rows > g.rows)
			rows = g.rows;
		/* flow position, then the next lanes, then a new page */
		int l = lane, r = row;
		for (;;) {
			if (l >= g.lanes) {
				if (any)
					page++;
				memset(&pg, 0, sizeof pg);
				l = 0;
				r = 0;
				break;
			}
			if (r + rows <= g.rows)
				break;
			l++;
			r = 0;
		}
		for (int k = 0; k < rows; k++)
			pg.used[l][r + k] = 3;
		*p = (struct pack_pos){ page, l, r, 0, COLS, rows };
		lane = l;
		row = r + rows;
		any = true;
	}
	return any ? page + 1 : 0;
}

void pack_page_range(const struct pack_pos *pos, size_t n, int page, size_t *first, size_t *count)
{
	size_t f = n, c = 0;
	for (size_t i = 0; i < n; i++)
		if (pos[i].page == page) {
			if (f == n)
				f = i;
			c++;
		}
	*first = f;
	*count = c;
}

bool pack_can_swap(enum orient o, const int *sizes, size_t n, size_t i, size_t j)
{
	if (i >= n || j >= n || i == j)
		return false;
	struct pack_pos *a = malloc(n * sizeof *a), *b = malloc(n * sizeof *b);
	int *s = malloc(n * sizeof *s);
	bool ok = false;
	if (a && b && s) {
		memcpy(s, sizes, n * sizeof *s);
		pack(o, s, n, a);
		int t = s[i];
		s[i] = s[j];
		s[j] = t;
		pack(o, s, n, b);
		ok = true;
		/* after the swap, slot i holds the widget that was at j and vice versa */
		for (size_t k = 0; k < n; k++)
			if (a[k].page != b[k].page)
				ok = false;
	}
	free(a);
	free(b);
	free(s);
	return ok;
}

bool pack_swap_pages(enum orient o, const int *sizes, size_t n, int a, int b, size_t *order)
{
	struct pack_pos *pos = malloc((n ? n : 1) * sizeof *pos);
	if (!pos)
		return false;
	int pages = pack(o, sizes, n, pos);
	if (a < 0 || b < 0 || a >= pages || b >= pages) {
		free(pos);
		return false;
	}
	if (a > b) {
		int t = a;
		a = b;
		b = t;
	}
	size_t fa, ca, fb, cb, k = 0;
	pack_page_range(pos, n, a, &fa, &ca);
	pack_page_range(pos, n, b, &fb, &cb);
	free(pos);
	for (size_t i = 0; i < fa; i++) order[k++] = i;
	for (size_t i = 0; i < cb; i++) order[k++] = fb + i;
	for (size_t i = fa + ca; i < fb; i++) order[k++] = i;
	for (size_t i = 0; i < ca; i++) order[k++] = fa + i;
	for (size_t i = fb + cb; i < n; i++) order[k++] = i;
	return true;
}
