/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * When widgets run, and what a run's result means for the card. Pure functions;
 * the event loop (timers, inotify) calls them.
 *
 * - Intervals sit on a shared grid aligned to local time (multiples of 15 minutes),
 *   so widgets with the same interval run together and their cards redraw in one
 *   partial refresh. No jitter.
 * - On change: Home watches the parent directories of the files the last run asked
 *   for (writers replace files by rename) and matches events by name.
 * - Events are debounced: a burst of writes gives one run.
 * - Results: a changed output hash redraws the card; a failure keeps the last good
 *   output; three failures in a row pause the widget until the gallery is opened.
 */
#ifndef GH_SCHED_H
#define GH_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sandbox.h"

#define PAUSE_AFTER_FAILURES 3

/* The next grid time strictly after now. every_s is a multiple of 900; tz_off_s is the local UTC offset. */
long long sched_next(long long now, long every_s, long tz_off_s);

struct debounce {
	bool pending;
	long long first_ms, due_ms;
};
/* A change event: the run is due quiet_ms after the last event, at most max_ms after the first. */
void debounce_event(struct debounce *d, long long now_ms, int quiet_ms, int max_ms);
/* True once when the debounced run is due. */
bool debounce_fire(struct debounce *d, long long now_ms);

/* The distinct parent directories of files (absolute paths); returns how many were written to dirs. */
size_t watch_dirs(char *const *files, size_t n, char **dirs, size_t max);
/* Does an event for NAME in directory DIR concern a run that read these files? */
bool watch_match(char *const *files, size_t n, const char *dir, const char *name);

enum trigger { TRIGGER_APPEAR, TRIGGER_INTERVAL, TRIGGER_CHANGE, TRIGGER_SETTINGS };

struct widget_state {
	int fails;           /* failures in a row */
	bool paused;
	bool has_output;     /* a good output exists */
	uint64_t hash;       /* of the last good output */
	enum run_status last;
	long long next_due;  /* next interval run, 0 = none */
};

enum outcome {
	OUTCOME_UNCHANGED, /* nothing to redraw */
	OUTCOME_CHANGED,   /* redraw the card with the new output */
	OUTCOME_FAILED,    /* keep the last good output; the meta line says the widget failed */
	OUTCOME_PAUSED,    /* the third failure in a row: no more runs until the gallery is opened */
};

/* Should a trigger run the widget now? Paused widgets don't run; settings changes always do. */
bool state_should_run(const struct widget_state *s, enum trigger t);
/* Records a run's status and output hash. */
enum outcome state_record(struct widget_state *s, enum run_status status, uint64_t hash);
/* Opening the gallery gives paused widgets another chance. */
void state_gallery_opened(struct widget_state *s);

#endif
