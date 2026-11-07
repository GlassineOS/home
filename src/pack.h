/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Page packing. Pages are never stored: they follow from the ordered widget list.
 *
 * Grid: portrait = one lane of 4 columns x 5 rows; landscape = two lanes of 4 x 3.
 * A half widget takes 2 columns, a wide one 4; both are one row tall. App widgets
 * are 4 columns and 1, 2, 3 or 5 rows; landscape caps them at 3 rows.
 *
 * Rules, in list order:
 *  - A half goes into the first free half cell of the current page (lanes in order,
 *    rows top to bottom, left before right). That can be a gap before the flow
 *    position: a half fills the free half beside an earlier half.
 *  - Anything 4 columns wide goes at the flow position (the row after the last
 *    row used in the current lane) if it fits, else at the top of the next lane,
 *    else at the top of a new page. Full-width widgets never move back into gaps,
 *    so their reading order is the list order.
 *  - A page exists only when a widget is on it, so empty pages close up.
 */
#ifndef GH_PACK_H
#define GH_PACK_H

#include <stdbool.h>
#include <stddef.h>

enum orient { ORIENT_PORTRAIT, ORIENT_LANDSCAPE };

/* A widget's size: SIZE_HALF, SIZE_WIDE, or an app widget's row count 1, 2, 3, 5. */
enum { SIZE_HALF = -1, SIZE_WIDE = -2 };

struct grid { int lanes, rows, cols; };
struct grid pack_grid(enum orient o);

/* Where a widget lands. rows is its height after the landscape cap. */
struct pack_pos { int page, lane, row, col, cols, rows; };

/* True for -1, -2, 1, 2, 3, 5. */
bool pack_size_valid(int size);

/* Packs n widgets; returns the number of pages (0 for none). out has n entries. */
int pack(enum orient o, const int *sizes, size_t n, struct pack_pos *out);

/* Index range [first, first+count) of the widgets on a page; pages are contiguous in list order. */
void pack_page_range(const struct pack_pos *pos, size_t n, int page, size_t *first, size_t *count);

/* True if swapping widgets i and j keeps every widget on the page it is on now. */
bool pack_can_swap(enum orient o, const int *sizes, size_t n, size_t i, size_t j);

/*
 * The list order after exchanging the widgets of pages a and b (as packed now):
 * writes a permutation of 0..n-1 to order. Returns false for a bad page number.
 */
bool pack_swap_pages(enum orient o, const int *sizes, size_t n, int a, int b, size_t *order);

#endif
