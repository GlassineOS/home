// SPDX-License-Identifier: GPL-2.0-or-later
#include "sched.h"

#include <stdlib.h>
#include <string.h>

long long sched_next(long long now, long every_s, long tz_off_s)
{
	if (every_s <= 0)
		return 0;
	long long local = now + tz_off_s;
	long long q = local / every_s;
	if (local < 0 && local % every_s)
		q--;
	return (q + 1) * every_s - tz_off_s;
}

void debounce_event(struct debounce *d, long long now_ms, int quiet_ms, int max_ms)
{
	if (!d->pending) {
		d->pending = true;
		d->first_ms = now_ms;
	}
	long long due = now_ms + quiet_ms;
	if (due > d->first_ms + max_ms)
		due = d->first_ms + max_ms;
	d->due_ms = due;
}

bool debounce_fire(struct debounce *d, long long now_ms)
{
	if (!d->pending || now_ms < d->due_ms)
		return false;
	d->pending = false;
	return true;
}

size_t watch_dirs(char *const *files, size_t n, char **dirs, size_t max)
{
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		const char *slash = strrchr(files[i], '/');
		if (!slash)
			continue;
		size_t len = slash == files[i] ? 1 : (size_t)(slash - files[i]);
		bool seen = false;
		for (size_t j = 0; j < k; j++)
			if (strlen(dirs[j]) == len && !strncmp(dirs[j], files[i], len))
				seen = true;
		if (seen || k == max)
			continue;
		char *d = malloc(len + 1);
		if (!d)
			continue;
		memcpy(d, files[i], len);
		d[len] = 0;
		dirs[k++] = d;
	}
	return k;
}

bool watch_match(char *const *files, size_t n, const char *dir, const char *name)
{
	size_t dl = strlen(dir), nl = strlen(name);
	if (dl > 1 && dir[dl - 1] == '/')
		dl--;
	for (size_t i = 0; i < n; i++) {
		const char *f = files[i];
		if (strncmp(f, dir, dl) || f[dl] != '/')
			continue;
		const char *rest = f + dl + 1;
		/* the file itself, or a directory on its way that was replaced */
		if (!strncmp(rest, name, nl) && (rest[nl] == 0 || rest[nl] == '/'))
			return true;
	}
	return false;
}

bool state_should_run(const struct widget_state *s, enum trigger t)
{
	return t == TRIGGER_SETTINGS || !s->paused;
}

enum outcome state_record(struct widget_state *s, enum run_status status, uint64_t hash)
{
	s->last = status;
	if (status == RUN_OK) {
		s->fails = 0;
		s->paused = false;
		if (s->has_output && s->hash == hash)
			return OUTCOME_UNCHANGED;
		s->has_output = true;
		s->hash = hash;
		return OUTCOME_CHANGED;
	}
	if (++s->fails >= PAUSE_AFTER_FAILURES) {
		s->paused = true;
		return OUTCOME_PAUSED;
	}
	return OUTCOME_FAILED;
}

void state_gallery_opened(struct widget_state *s)
{
	s->paused = false;
	s->fails = 0;
}
