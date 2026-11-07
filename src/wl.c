// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * glassine-home on Wayland: one xdg_toplevel (app_id glassine-home), shared-memory
 * buffers, touch, pointer and keyboard. The session (GlassineOS/shell's swayd) keeps
 * it in sway's scratchpad and shows it floating over the work area; Home never
 * fullscreens itself and hides only through `glassine-swayd home hide`.
 *
 * Every paint of the core ends in at most one commit: the damaged rectangles are
 * copied from the core's canvas into a free buffer (which also gets what changed
 * since it was last used) and committed together. With both buffers still with the
 * compositor, the paint is owed (frames.c) and committed from the main loop once one
 * is released: commit never dispatches events, so no handler runs in a paint.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "home.h"
#include "xdg-shell-client-protocol.h"

struct buffer {
	struct wl_buffer *b;
	void *data;
	int w, h, stride;
	size_t size;
};

static struct {
	struct wl_display *d;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct wl_seat *seat;
	struct wl_output *out;
	struct wl_surface *surf;
	struct xdg_surface *xs;
	struct xdg_toplevel *top;
	struct wl_pointer *ptr;
	struct wl_touch *touch;
	struct wl_keyboard *kbd;
	struct xkb_context *xkb;
	struct xkb_keymap *km;
	struct xkb_state *ks;
	int out_scale, mode_w, mode_h;
	int cw, ch;              /* configured size, surface px */
	bool configured, activated, running;
	double px, py;
	int touch_id;
	struct buffer buf[FRAME_BUFFERS];
	struct frames f;         /* which buffer is free, what each copies and the surface damages */
	struct home *h;
} W = { .out_scale = 1, .touch_id = -1, .running = true };

/* ---- buffers ----------------------------------------------------------------------- */

static void buf_release(void *data, struct wl_buffer *b)
{
	frames_released(&W.f, (int)((struct buffer *)data - W.buf));
}
static const struct wl_buffer_listener buf_listener = { buf_release };

static void wlbuf_free(struct buffer *b)
{
	if (b->b)
		wl_buffer_destroy(b->b);
	if (b->data)
		munmap(b->data, b->size);
	memset(b, 0, sizeof *b);
}

static bool buf_make(struct buffer *b, int w, int h)
{
	wlbuf_free(b);
	b->w = w;
	b->h = h;
	b->stride = w * 4;
	b->size = (size_t)b->stride * h;
	int fd = memfd_create("glassine-home", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)b->size)) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED) {
		b->data = NULL;
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(W.shm, fd, (int)b->size);
	b->b = wl_shm_pool_create_buffer(pool, 0, w, h, b->stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->b, &buf_listener, b);
	return true;
}

/* the canvas is the buffer's size here (present), so a rect clipped to one fits both */
static void copy_rect(struct buffer *b, const unsigned char *src, int sstride, const struct rect *r)
{
	int x0, y0, x1, y1;
	if (!frames_clip(r, b->w, b->h, &x0, &y0, &x1, &y1))
		return;
	for (int y = y0; y < y1; y++)
		memcpy((char *)b->data + (size_t)y * b->stride + x0 * 4, src + (size_t)y * sstride + x0 * 4, (size_t)(x1 - x0) * 4);
}

/* Commits the canvas as it is now into a free buffer; with none free, the frame stays owed. */
static void present(void)
{
	struct home *h = W.h;
	if (!W.configured || !h->cur)
		return; /* a new canvas is coming, and its paint damages everything */
	int dw = cairo_image_surface_get_width(h->cur), dh = cairo_image_surface_get_height(h->cur);
	frames_canvas(&W.f, dw, dh);
	int i = frames_pick(&W.f);
	if (i < 0)
		return;
	struct buffer *b = &W.buf[i];
	if (b->w != dw || b->h != dh) {
		if (!buf_make(b, dw, dh))
			return;
		W.f.b[i].all = true;
	}
	const unsigned char *src = cairo_image_surface_get_data(h->cur);
	int sstride = cairo_image_surface_get_stride(h->cur);
	struct rect full = { 0, 0, dw, dh };
	if (W.f.b[i].all) {
		copy_rect(b, src, sstride, &full);
	} else {
		for (int k = 0; k < W.f.b[i].ndmg; k++)
			copy_rect(b, src, sstride, &W.f.b[i].dmg[k]);
	}
	wl_surface_attach(W.surf, b->b, 0, 0);
	const struct rect *sd = W.f.sall ? &full : W.f.sdmg;
	int nsd = W.f.sall ? 1 : W.f.nsdmg;
	for (int k = 0; k < nsd; k++)
		wl_surface_damage_buffer(W.surf, (int)sd[k].x, (int)sd[k].y, (int)ceil(sd[k].w), (int)ceil(sd[k].h));
	frames_shown(&W.f, i);
	wl_surface_commit(W.surf);
}

static void commit(void *ud, const struct rect *r, int n)
{
	if (!W.configured || !W.h->cur)
		return;
	frames_damage(&W.f, r, n);
	present();
}

/* ---- size ------------------------------------------------------------------------------- */

/* device px per design px: the panel's short side over the design's 702 (as the shell) */
static int scale(void)
{
	const char *e = getenv("GLASSINE_HOME_SCALE");
	if (e && atoi(e) > 0)
		return atoi(e);
	if (W.mode_w > 0 && W.mode_h > 0) {
		int sh = W.mode_w < W.mode_h ? W.mode_w : W.mode_h;
		int s = (int)lround(sh / 702.0);
		return s < 1 ? 1 : s;
	}
	return W.out_scale;
}

static void apply_size(void)
{
	int cs = W.out_scale < 1 ? 1 : W.out_scale;
	int bw = (W.cw ? W.cw : 702) * cs, bh = (W.ch ? W.ch : 884) * cs;
	int S = scale();
	wl_surface_set_buffer_scale(W.surf, cs);
	home_resize(W.h, bw / S, bh / S, S);
}

/* ---- xdg ------------------------------------------------------------------------------- */

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial)
{
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	W.configured = true;
	apply_size();
	W.h->dirty_now = true;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
	if (w > 0)
		W.cw = w;
	if (h > 0)
		W.ch = h;
	bool act = false;
	uint32_t *s;
	wl_array_for_each(s, states)
		if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
			act = true;
	if (act && !W.activated)
		home_appear(W.h); /* shown: run every widget once */
	W.activated = act;
}

static void top_close(void *d, struct xdg_toplevel *t)
{
	W.running = false;
}

static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

/* ---- input --------------------------------------------------------------------------- */

static double lx(double v)
{
	return v * (W.out_scale < 1 ? 1 : W.out_scale) / W.h->S;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{
	W.px = lx(wl_fixed_to_double(x));
	W.py = lx(wl_fixed_to_double(y));
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{
	home_cancel(W.h);
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	W.px = lx(wl_fixed_to_double(x));
	W.py = lx(wl_fixed_to_double(y));
}
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{
	if (b != BTN_LEFT)
		return;
	if (st == WL_POINTER_BUTTON_STATE_PRESSED)
		home_press(W.h, W.px, W.py);
	else
		home_release(W.h, W.px, W.py);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
/* seat version 5: the later events are never sent */
static const struct wl_pointer_listener ptr_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
	.frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};

static void t_down(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, struct wl_surface *sf, int32_t id, wl_fixed_t x,
		   wl_fixed_t y)
{
	if (W.touch_id >= 0)
		return; /* one finger */
	W.touch_id = id;
	W.px = lx(wl_fixed_to_double(x));
	W.py = lx(wl_fixed_to_double(y));
	home_press(W.h, W.px, W.py);
}
static void t_up(void *d, struct wl_touch *t, uint32_t s, uint32_t tm, int32_t id)
{
	if (id != W.touch_id)
		return;
	W.touch_id = -1;
	home_release(W.h, W.px, W.py);
}
static void t_motion(void *d, struct wl_touch *t, uint32_t tm, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	if (id != W.touch_id)
		return;
	W.px = lx(wl_fixed_to_double(x));
	W.py = lx(wl_fixed_to_double(y));
}
static void t_frame(void *d, struct wl_touch *t) {}
static void t_cancel(void *d, struct wl_touch *t)
{
	W.touch_id = -1;
	home_cancel(W.h);
}
static void t_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t a, wl_fixed_t b) {}
static void t_orient(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o) {}
static const struct wl_touch_listener touch_listener = { t_down, t_up, t_motion, t_frame, t_cancel, t_shape, t_orient };

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return;
	if (W.ks)
		xkb_state_unref(W.ks);
	if (W.km)
		xkb_keymap_unref(W.km);
	W.km = xkb_keymap_new_from_string(W.xkb, m, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(m, size);
	W.ks = W.km ? xkb_state_new(W.km) : NULL;
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) {}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {}
static void k_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t state)
{
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !W.ks)
		return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(W.ks, key + 8);
	bool shift = xkb_state_mod_name_is_active(W.ks, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
	switch (sym) {
	case XKB_KEY_Tab:
		home_key(W.h, shift ? K_BACKTAB : K_TAB);
		break;
	case XKB_KEY_ISO_Left_Tab:
		home_key(W.h, K_BACKTAB);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		home_key(W.h, K_ENTER);
		break;
	case XKB_KEY_space:
		home_key(W.h, K_SPACE);
		break;
	case XKB_KEY_Escape:
		home_key(W.h, K_ESCAPE);
		break;
	default:
		break;
	}
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
	if (W.ks)
		xkb_state_update_mask(W.ks, dep, lat, lock, 0, 0, grp);
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener kbd_listener = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !W.ptr) {
		W.ptr = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(W.ptr, &ptr_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !W.touch) {
		W.touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(W.touch, &touch_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !W.kbd) {
		W.kbd = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(W.kbd, &kbd_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
		       const char *mk, const char *md, int32_t tr) {}
static void o_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		W.mode_w = w;
		W.mode_h = h;
	}
}
static void o_done(void *d, struct wl_output *o)
{
	if (W.surf && W.configured)
		apply_size();
}
static void o_scale(void *d, struct wl_output *o, int32_t f)
{
	W.out_scale = f;
}
static const struct wl_output_listener out_listener = {
	.geometry = o_geometry, .mode = o_mode, .done = o_done, .scale = o_scale,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name))
		W.comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		W.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		W.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 4 ? ver : 4);
		xdg_wm_base_add_listener(W.wm, &wm_listener, NULL);
	} else if (!strcmp(iface, wl_seat_interface.name) && !W.seat) {
		W.seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
		wl_seat_add_listener(W.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wl_output_interface.name) && !W.out) {
		W.out = wl_registry_bind(r, name, &wl_output_interface, ver < 2 ? ver : 2);
		wl_output_add_listener(W.out, &out_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void on_term(int sig)
{
	W.running = false;
}

int home_wayland_main(struct zygote *z)
{
	static struct home h;
	struct home_io io = { .commit = commit };
	W.d = wl_display_connect(NULL);
	if (!W.d) {
		fprintf(stderr, "glassine-home: no Wayland display\n");
		return 1;
	}
	W.h = &h;
	frames_init(&W.f);
	home_init(&h, z, &io);
	ui_draw_init();
	W.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct wl_registry *reg = wl_display_get_registry(W.d);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(W.d);
	wl_display_roundtrip(W.d);
	if (!W.comp || !W.shm || !W.wm) {
		fprintf(stderr, "glassine-home: the compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
		return 1;
	}
	W.surf = wl_compositor_create_surface(W.comp);
	W.xs = xdg_wm_base_get_xdg_surface(W.wm, W.surf);
	xdg_surface_add_listener(W.xs, &xs_listener, NULL);
	W.top = xdg_surface_get_toplevel(W.xs);
	xdg_toplevel_add_listener(W.top, &top_listener, NULL);
	const char *id = getenv("GLASSINE_HOME_APP_ID");
	xdg_toplevel_set_app_id(W.top, id && *id ? id : "glassine-home");
	xdg_toplevel_set_title(W.top, "Home");
	wl_surface_commit(W.surf);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	signal(SIGPIPE, SIG_IGN);
	home_appear(&h);

	int status = 0;
	while (W.running) {
		int wait = home_tick(&h);
		wl_display_flush(W.d);
		struct pollfd p[8];
		int fds[6], nf = home_fds(&h, fds, 6);
		p[0] = (struct pollfd){ wl_display_get_fd(W.d), POLLIN, 0 };
		for (int i = 0; i < nf; i++)
			p[1 + i] = (struct pollfd){ fds[i], POLLIN, 0 };
		while (wl_display_prepare_read(W.d) != 0)
			wl_display_dispatch_pending(W.d);
		wl_display_flush(W.d);
		int r = poll(p, 1 + nf, wait);
		if (r > 0 && (p[0].revents & POLLIN))
			wl_display_read_events(W.d);
		else
			wl_display_cancel_read(W.d);
		if (wl_display_dispatch_pending(W.d) < 0)
			break;
		if (p[0].revents & (POLLHUP | POLLERR))
			break;
		if (W.f.owed)
			present(); /* a buffer may be back: commit the paint that waited for one */
		for (int i = 0; r > 0 && i < nf; i++)
			if (p[1 + i].revents)
				home_ready(&h, p[1 + i].fd);
		if (home_helper_gone(&h)) {
			fprintf(stderr, "glassine-home: the widget helper stopped; exiting, for the session to start Home again\n");
			status = 1;
			break;
		}
	}
	home_free(&h);
	for (int i = 0; i < FRAME_BUFFERS; i++)
		wlbuf_free(&W.buf[i]);
	wl_display_disconnect(W.d);
	return status;
}
