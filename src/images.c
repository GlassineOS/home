// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Image slots (spec 001 FR-027): a worker thread loads a slot's source, scales it to
 * exactly the slot (cover or contain), turns it grey and, when the E-ink dithering
 * setting is on, dithers it to the panel's 16 greys with the chosen pattern. Results
 * are cached in memory and in $XDG_CACHE_HOME/glassine-home/images, keyed by the
 * source, its modification time and size, the slot size, the fit and the dithering.
 *
 * Sources: file:PATH (a file the widget's reads cover; the description check made
 * sure), cover:BOOK (the book's freedesktop thumbnail), icon:APP (the app's icon,
 * drawn on the main thread: the icon index belongs to it). Over 20 MB or broken: the
 * slot keeps its dashed frame and alt text.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "home.h"

struct job {
	char key[64];
	int fd;                /* the source, opened by the main thread */
	int w, h;
	bool contain;
	int dither;            /* 0 off, 1 blue, 2 bayer, 3 dizzy */
	/* result */
	unsigned char *grey;   /* w*h, 0 = black */
	bool failed;
	struct job *next;
};

struct imgq {
	pthread_t th;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	struct job *todo, *done;
	int efd;
	bool stop, started;
	/* finished pictures by key, so a re-render with the same slot costs nothing */
	struct { char key[64]; cairo_surface_t *s; bool failed; } cache[64];
	int ncache, next_evict;
};

/* ---- the pixel work (worker thread) ----------------------------------------------- */

static const unsigned char BAYER8[64] = {
	0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38,
	60, 28, 52, 20, 62, 30, 54, 22, 3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
	15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21,
};

/* 16 greys (the panel's 4 bits): level step 17 */
static void dither(unsigned char *g, int w, int h, int mode)
{
	if (mode == 3) { /* error diffusion (Floyd-Steinberg): "dizzy" */
		float *err = calloc((size_t)(w + 2) * 2, sizeof *err);
		if (!err)
			return;
		float *cur = err + 1, *nxt = err + w + 3;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float v = g[y * w + x] + cur[x];
				int q = (int)(v / 17 + 0.5f);
				q = q < 0 ? 0 : q > 15 ? 15 : q;
				g[y * w + x] = (unsigned char)(q * 17);
				float e = v - q * 17;
				cur[x + 1] += e * 7 / 16;
				nxt[x - 1] += e * 3 / 16;
				nxt[x] += e * 5 / 16;
				nxt[x + 1] += e * 1 / 16;
			}
			float *t = cur;
			cur = nxt;
			nxt = t;
			memset(nxt - 1, 0, (size_t)(w + 2) * sizeof *nxt);
		}
		free(err);
		return;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			float t;
			if (mode == 2) {
				t = (BAYER8[(y & 7) * 8 + (x & 7)] + 0.5f) / 64;
			} else { /* "blue": interleaved gradient noise (an ordered noise, not true blue noise) */
				float f = 52.9829189f * (0.06711056f * x + 0.00583715f * y);
				t = f - (float)(long)f;
			}
			float v = g[y * w + x] / 17.0f;
			int q = (int)v + (v - (int)v > t);
			g[y * w + x] = (unsigned char)((q > 15 ? 15 : q) * 17);
		}
}

struct fit { int w, h; bool contain; };

static void size_prepared(GdkPixbufLoader *l, int sw, int sh, gpointer ud)
{
	struct fit *f = ud;
	if (sw <= 0 || sh <= 0)
		return;
	double sx = (double)f->w / sw, sy = (double)f->h / sh, sc = f->contain ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
	int tw = (int)(sw * sc + 0.5), th = (int)(sh * sc + 0.5);
	gdk_pixbuf_loader_set_size(l, tw < 1 ? 1 : tw, th < 1 ? 1 : th);
}

static unsigned char *prepare(int fd, int w, int h, bool contain, int dmode)
{
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > IMAGE_MAX_BYTES)
		return NULL;
	unsigned char *data = malloc((size_t)st.st_size + 1);
	size_t got = 0;
	while (data && got < (size_t)st.st_size) {
		ssize_t n = read(fd, data + got, (size_t)st.st_size - got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	if (!data || got != (size_t)st.st_size) {
		free(data);
		return NULL;
	}
	struct fit f = { w, h, contain };
	GdkPixbufLoader *l = gdk_pixbuf_loader_new();
	g_signal_connect(l, "size-prepared", G_CALLBACK(size_prepared), &f);
	GError *err = NULL;
	bool ok = gdk_pixbuf_loader_write(l, data, got, &err);
	ok = gdk_pixbuf_loader_close(l, ok ? &err : NULL) && ok;
	if (err) {
		fprintf(stderr, "glassine-home: an image: %s\n", err->message);
		g_error_free(err);
	}
	free(data);
	GdkPixbuf *pb = ok ? gdk_pixbuf_loader_get_pixbuf(l) : NULL;
	if (pb)
		g_object_ref(pb);
	g_object_unref(l);
	if (!pb)
		return NULL;
	unsigned char *g = malloc((size_t)w * h);
	if (!g) {
		g_object_unref(pb);
		return NULL;
	}
	memset(g, 255, (size_t)w * h);
	int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb), rs = gdk_pixbuf_get_rowstride(pb);
	int nc = gdk_pixbuf_get_n_channels(pb);
	bool alpha = gdk_pixbuf_get_has_alpha(pb);
	const unsigned char *px = gdk_pixbuf_read_pixels(pb);
	int ox = (w - pw) / 2, oy = (h - ph) / 2; /* cover: negative, the middle is kept */
	for (int y = 0; y < h; y++) {
		int py = y - oy;
		if (py < 0 || py >= ph)
			continue;
		for (int x = 0; x < w; x++) {
			int qx = x - ox;
			if (qx < 0 || qx >= pw)
				continue;
			const unsigned char *p = px + (size_t)py * rs + (size_t)qx * nc;
			/* luminance (Rec. 709), over white */
			float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
			if (alpha)
				l = l * p[3] / 255.0f + 255.0f * (1 - p[3] / 255.0f);
			g[y * w + x] = (unsigned char)(l + 0.5f);
		}
	}
	g_object_unref(pb);
	if (dmode)
		dither(g, w, h, dmode);
	return g;
}

static bool cache_dir(char *out, size_t len)
{
	const char *x = getenv("XDG_CACHE_HOME"), *hm = getenv("HOME");
	if (x && x[0] == '/')
		snprintf(out, len, "%s/glassine-home/images", x);
	else if (hm && hm[0] == '/')
		snprintf(out, len, "%s/.cache/glassine-home/images", hm);
	else
		return false;
	return true;
}

static void mkdirs(const char *p)
{
	char b[1100];
	snprintf(b, sizeof b, "%s", p);
	for (char *s = b + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(b, 0755);
			*s = '/';
		}
	mkdir(b, 0755);
}

/* the disk cache: a binary PGM per key */
static unsigned char *disk_get(const char *key, int w, int h)
{
	char d[1024], p[1200];
	if (!cache_dir(d, sizeof d))
		return NULL;
	snprintf(p, sizeof p, "%s/%s.pgm", d, key);
	FILE *f = fopen(p, "rbe");
	if (!f)
		return NULL;
	int fw, fh, mx;
	unsigned char *g = NULL;
	if (fscanf(f, "P5 %d %d %d", &fw, &fh, &mx) == 3 && fw == w && fh == h && mx == 255 && fgetc(f) != EOF) {
		g = malloc((size_t)w * h);
		if (g && fread(g, 1, (size_t)w * h, f) != (size_t)w * h) {
			free(g);
			g = NULL;
		}
	}
	fclose(f);
	return g;
}

static void disk_put(const char *key, const unsigned char *g, int w, int h)
{
	char d[1024], p[1200], t[1300];
	if (!cache_dir(d, sizeof d))
		return;
	mkdirs(d);
	snprintf(p, sizeof p, "%s/%s.pgm", d, key);
	snprintf(t, sizeof t, "%s.tmp", p);
	FILE *f = fopen(t, "wbe");
	if (!f)
		return;
	fprintf(f, "P5 %d %d 255\n", w, h);
	bool ok = fwrite(g, 1, (size_t)w * h, f) == (size_t)w * h;
	if (fclose(f) == 0 && ok)
		rename(t, p);
	else
		unlink(t);
}

static void *worker(void *arg)
{
	struct imgq *q = arg;
	pthread_mutex_lock(&q->mu);
	for (;;) {
		while (!q->todo && !q->stop)
			pthread_cond_wait(&q->cv, &q->mu);
		if (q->stop)
			break;
		struct job *j = q->todo;
		q->todo = j->next;
		pthread_mutex_unlock(&q->mu);
		j->grey = disk_get(j->key, j->w, j->h);
		if (!j->grey) {
			j->grey = prepare(j->fd, j->w, j->h, j->contain, j->dither);
			if (j->grey)
				disk_put(j->key, j->grey, j->w, j->h);
		}
		j->failed = !j->grey;
		close(j->fd);
		pthread_mutex_lock(&q->mu);
		j->next = q->done;
		q->done = j;
		uint64_t one = 1;
		if (write(q->efd, &one, sizeof one) < 0) {
			/* the loop drains the counter; nothing to do */
		}
	}
	pthread_mutex_unlock(&q->mu);
	return NULL;
}

/* ---- the main thread's side ------------------------------------------------------------ */

struct imgq *imgq_start(void)
{
	struct imgq *q = calloc(1, sizeof *q);
	if (!q)
		return NULL;
	pthread_mutex_init(&q->mu, NULL);
	pthread_cond_init(&q->cv, NULL);
	q->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	q->started = pthread_create(&q->th, NULL, worker, q) == 0;
	return q;
}

void imgq_stop(struct imgq *q)
{
	if (!q)
		return;
	pthread_mutex_lock(&q->mu);
	q->stop = true;
	pthread_cond_signal(&q->cv);
	pthread_mutex_unlock(&q->mu);
	if (q->started)
		pthread_join(q->th, NULL);
	for (struct job *j = q->todo, *n; j; j = n) {
		n = j->next;
		close(j->fd);
		free(j);
	}
	for (struct job *j = q->done, *n; j; j = n) {
		n = j->next;
		free(j->grey);
		free(j);
	}
	for (int i = 0; i < q->ncache; i++)
		if (q->cache[i].s)
			cairo_surface_destroy(q->cache[i].s);
	if (q->efd >= 0)
		close(q->efd);
	free(q);
}

int imgq_fd(struct imgq *q)
{
	return q ? q->efd : -1;
}

void img_clear(struct img *im)
{
	if (im->surf)
		cairo_surface_destroy(im->surf);
	memset(im, 0, sizeof *im);
}

static cairo_surface_t *surface_of(const unsigned char *g, int w, int h)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS)
		return NULL;
	cairo_surface_flush(s);
	unsigned char *d = cairo_image_surface_get_data(s);
	int st = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t v = g[y * w + x];
			((uint32_t *)(d + (size_t)y * st))[x] = 0xff000000u | v << 16 | v << 8 | v;
		}
	cairo_surface_mark_dirty(s);
	return s;
}

static void cache_put(struct imgq *q, const char *key, cairo_surface_t *s, bool failed)
{
	int i = q->ncache < 64 ? q->ncache++ : q->next_evict++ % 64;
	if (q->cache[i].s)
		cairo_surface_destroy(q->cache[i].s);
	snprintf(q->cache[i].key, sizeof q->cache[i].key, "%s", key);
	q->cache[i].s = s ? cairo_surface_reference(s) : NULL;
	q->cache[i].failed = failed;
}

static int cache_find(struct imgq *q, const char *key)
{
	for (int i = 0; i < q->ncache; i++)
		if (!strcmp(q->cache[i].key, key))
			return i;
	return -1;
}

bool img_cover_path(const char *book, char *out, size_t len)
{
	gchar *uri = g_filename_to_uri(book, NULL, NULL);
	if (!uri)
		return false;
	gchar *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
	g_free(uri);
	const char *x = getenv("XDG_CACHE_HOME"), *hm = getenv("HOME");
	char base[1024];
	if (x && x[0] == '/')
		snprintf(base, sizeof base, "%s/thumbnails", x);
	else
		snprintf(base, sizeof base, "%s/.cache/thumbnails", hm ? hm : "");
	static const char *const sizes[] = { "xx-large", "x-large", "large", "normal" };
	bool ok = false;
	for (int i = 0; i < 4 && !ok; i++) {
		snprintf(out, len, "%s/%s/%s.png", base, sizes[i], md5);
		ok = access(out, R_OK) == 0;
	}
	g_free(md5);
	return ok;
}

static int dither_mode(const struct home *h)
{
	if (!h->dither)
		return 0;
	return !strcmp(h->dither_pattern, "bayer") ? 2 : !strcmp(h->dither_pattern, "dizzy") ? 3 : 1;
}

/* icon:APP, drawn here with the voice's icon lookup, then greyed like the rest */
static cairo_surface_t *icon_picture(struct home *h, const char *id, int w, int hh, int dmode)
{
	const struct app *a = apps_resolve(&h->apps, id);
	if (!a || !a->icon[0])
		return NULL;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, hh);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	UiGc g = { cr, 1, h->v };
	int px = w < hh ? w : hh;
	bool ok = ui_icon(&g, a->icon, px, w / 2.0, hh / 2.0, h->v->ink);
	cairo_destroy(cr);
	cairo_surface_flush(s);
	if (!ok) {
		cairo_surface_destroy(s);
		return NULL;
	}
	unsigned char *gr = malloc((size_t)w * hh), *d = cairo_image_surface_get_data(s);
	int st = cairo_image_surface_get_stride(s);
	for (int y = 0; gr && y < hh; y++)
		for (int x = 0; x < w; x++) {
			uint32_t p = ((uint32_t *)(d + (size_t)y * st))[x];
			gr[y * w + x] = (unsigned char)(0.2126f * (p >> 16 & 255) + 0.7152f * (p >> 8 & 255) + 0.0722f * (p & 255));
		}
	cairo_surface_destroy(s);
	if (!gr)
		return NULL;
	if (dmode)
		dither(gr, w, hh, dmode);
	cairo_surface_t *out = surface_of(gr, w, hh);
	free(gr);
	return out;
}

void img_request(struct home *h, struct card *c, int i, const struct value *item, int lw, int lh)
{
	struct imgq *q = h->iq;
	if (!q || i < 0 || i >= DESC_MAX_IMAGES || lw <= 0 || lh <= 0)
		return;
	const struct value *src = v_map_get(item, "src"), *fit = v_map_get(item, "fit");
	if (!src || src->t != V_STR)
		return;
	int w = lw * h->S, hh = lh * h->S, dmode = dither_mode(h);
	bool contain = fit && fit->t == V_STR && !strcmp(fit->u.s.p, "contain");
	const char *s = src->u.s.p;
	char path[1100] = "";
	long long mtime = 0, size = 0;
	if (!strncmp(s, "file:", 5)) {
		snprintf(path, sizeof path, "%s", s + 5);
	} else if (!strncmp(s, "cover:", 6)) {
		if (!img_cover_path(s + 6, path, sizeof path))
			path[0] = 0;
	} else if (strncmp(s, "icon:", 5)) {
		return;
	}
	/* file: is a file the widget's reads cover (checked when its output was read); open
	 * it here without following any link on the way, so a link can't lead elsewhere */
	int fd = -1;
	if (path[0]) {
		fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
		char real[1100], link[64];
		snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
		ssize_t n = fd >= 0 ? readlink(link, real, sizeof real - 1) : -1;
		if (n > 0)
			real[n] = 0;
		if (fd >= 0 && !strncmp(s, "file:", 5) && (n <= 0 || strcmp(real, path))) {
			close(fd);
			fd = -1;
		}
	}
	struct stat st;
	if (fd >= 0 && fstat(fd, &st) == 0) {
		mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
		size = (long long)st.st_size;
	}
	char kb[1500];
	snprintf(kb, sizeof kb, "%s|%s|%lld|%lld|%dx%d|%d|%d", s, path, mtime, size, w, hh, contain, dmode);
	char key[64];
	snprintf(key, sizeof key, "%016llx", (unsigned long long)hash64(kb, strlen(kb)));
	struct img *im = &c->img[i];
	if (!strcmp(im->key, key) && (im->surf || im->queued || im->failed)) {
		if (fd >= 0)
			close(fd);
		return; /* the same picture: nothing to do */
	}
	img_clear(im);
	snprintf(im->key, sizeof im->key, "%s", key);
	im->w = w;
	im->h = hh;
	int ci = cache_find(q, key);
	if (ci >= 0 && fd >= 0)
		close(fd);
	if (ci >= 0) {
		im->surf = q->cache[ci].s ? cairo_surface_reference(q->cache[ci].s) : NULL;
		im->failed = q->cache[ci].failed;
		return;
	}
	if (!strncmp(s, "icon:", 5)) {
		im->surf = icon_picture(h, s + 5, w, hh, dmode);
		im->failed = !im->surf;
		cache_put(q, key, im->surf, im->failed);
		return;
	}
	if (fd < 0) {
		im->failed = true;
		return;
	}
	struct job *j = calloc(1, sizeof *j);
	if (!j) {
		close(fd);
		return;
	}
	snprintf(j->key, sizeof j->key, "%s", key);
	j->fd = fd;
	j->w = w;
	j->h = hh;
	j->contain = contain;
	j->dither = dmode;
	im->queued = true;
	pthread_mutex_lock(&q->mu);
	struct job **tail = &q->todo;
	while (*tail)
		tail = &(*tail)->next;
	*tail = j;
	pthread_cond_signal(&q->cv);
	pthread_mutex_unlock(&q->mu);
}

bool imgq_collect(struct home *h)
{
	struct imgq *q = h->iq;
	uint64_t n;
	if (read(q->efd, &n, sizeof n) < 0 && errno != EAGAIN)
		return false;
	pthread_mutex_lock(&q->mu);
	struct job *done = q->done;
	q->done = NULL;
	pthread_mutex_unlock(&q->mu);
	bool any = false;
	for (struct job *j = done, *nx; j; j = nx) {
		nx = j->next;
		cairo_surface_t *s = j->grey ? surface_of(j->grey, j->w, j->h) : NULL;
		cache_put(q, j->key, s, !s);
		for (size_t k = 0; k < h->ncards; k++)
			for (int m = 0; m < DESC_MAX_IMAGES; m++) {
				struct img *im = &h->cards[k].img[m];
				if (im->queued && !strcmp(im->key, j->key)) {
					im->queued = false;
					im->surf = s ? cairo_surface_reference(s) : NULL;
					im->failed = !s;
					any = true;
				}
			}
		if (s)
			cairo_surface_destroy(s);
		free(j->grey);
		free(j);
	}
	return any;
}
