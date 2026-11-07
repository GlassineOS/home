// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What wl.c commits: two buffers that take turns with the compositor, what each must
 * copy from the canvas when it is used next, and what the surface must damage. No
 * Wayland here, so the case of both buffers being busy can be tested without one.
 */
#include <string.h>

#include "home.h"

static void add(struct rect *list, int *n, bool *all, const struct rect *r)
{
	if (*all)
		return;
	if (*n < MAX_DAMAGE)
		list[(*n)++] = *r;
	else
		*all = true;
}

void frames_init(struct frames *f)
{
	memset(f, 0, sizeof *f);
	for (int i = 0; i < FRAME_BUFFERS; i++)
		f->b[i].all = true; /* never shown: copied whole */
}

void frames_canvas(struct frames *f, int w, int h)
{
	if (w == f->cw && h == f->ch)
		return;
	f->cw = w;
	f->ch = h;
	for (int i = 0; i < FRAME_BUFFERS; i++) {
		f->b[i].ndmg = 0;
		f->b[i].all = true;
	}
	f->nsdmg = 0;
	f->sall = true;
}

bool frames_clip(const struct rect *r, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
	double a = r->x < 0 ? 0 : r->x, b = r->y < 0 ? 0 : r->y;
	double c = r->x + r->w > w ? w : r->x + r->w, d = r->y + r->h > h ? h : r->y + r->h;
	if (!(c > a && d > b)) /* empty, outside, or not a number */
		return false;
	*x0 = (int)a;
	*y0 = (int)b;
	*x1 = (int)c;
	*y1 = (int)d;
	return *x1 > *x0 && *y1 > *y0;
}

void frames_damage(struct frames *f, const struct rect *r, int n)
{
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < FRAME_BUFFERS; k++)
			add(f->b[k].dmg, &f->b[k].ndmg, &f->b[k].all, &r[i]);
		add(f->sdmg, &f->nsdmg, &f->sall, &r[i]);
	}
}

int frames_pick(struct frames *f)
{
	for (int i = 0; i < FRAME_BUFFERS; i++)
		if (!f->b[i].busy)
			return i;
	f->owed = true;
	return -1;
}

void frames_shown(struct frames *f, int i)
{
	f->b[i].busy = true;
	f->b[i].ndmg = 0;
	f->b[i].all = false;
	f->nsdmg = 0;
	f->sall = false;
	f->owed = false;
}

void frames_released(struct frames *f, int i)
{
	f->b[i].busy = false;
}
