// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/sched.h"
#include "t.h"

#include <stdlib.h>

static void grid(void)
{
	/* 15 minutes: the next quarter hour, never now itself */
	CHECK_INT(sched_next(1000, 900, 0), 1800);
	CHECK_INT(sched_next(1800, 900, 0), 2700);
	/* one hour on local time: UTC+5:30 lands on local full hours */
	long long t = sched_next(1700000000, 3600, 19800);
	CHECK_INT((t + 19800) % 3600, 0);
	CHECK(t > 1700000000 && t <= 1700000000 + 3600);
	/* a day: local midnight */
	t = sched_next(1700000000, 86400, 3600);
	CHECK_INT((t + 3600) % 86400, 0);
	/* widgets with the same interval run at the same time, whenever they were added */
	CHECK_INT(sched_next(1700000123, 1800, 0), sched_next(1700000999, 1800, 0));
	CHECK_INT(sched_next(5, 0, 0), 0);
}

static void debounce(void)
{
	struct debounce d = { 0 };
	CHECK(!debounce_fire(&d, 0));
	debounce_event(&d, 1000, 300, 2000);
	CHECK(!debounce_fire(&d, 1200));
	debounce_event(&d, 1200, 300, 2000); /* a burst pushes it on */
	CHECK(!debounce_fire(&d, 1400));
	CHECK(debounce_fire(&d, 1500));
	CHECK(!debounce_fire(&d, 1600)); /* once */
	/* a constant stream still runs after max_ms */
	for (long long ms = 5000; ms < 8000; ms += 100) {
		debounce_event(&d, ms, 300, 2000);
		if (debounce_fire(&d, ms)) {
			CHECK_INT(ms, 7000);
			break;
		}
	}
}

static void watching(void)
{
	char *files[] = { "/h/.config/koreader/history.lua", "/h/Books/A.sdr/metadata.epub.lua", "/h/Books/B.sdr/metadata.pdf.lua" };
	char *dirs[8];
	size_t n = watch_dirs(files, 3, dirs, 8);
	CHECK_INT(n, 3);
	CHECK_STR(dirs[0], "/h/.config/koreader");
	CHECK_STR(dirs[1], "/h/Books/A.sdr");
	for (size_t i = 0; i < n; i++)
		free(dirs[i]);
	/* written by rename into the directory */
	CHECK(watch_match(files, 3, "/h/.config/koreader", "history.lua"));
	CHECK(!watch_match(files, 3, "/h/.config/koreader", "history.lua.tmp"));
	CHECK(!watch_match(files, 3, "/h/.config/koreader", "history"));
	/* a replaced directory on the way */
	CHECK(watch_match(files, 3, "/h/Books", "A.sdr"));
	CHECK(!watch_match(files, 3, "/h/Books", "A"));
	CHECK(watch_match(files, 3, "/h/Books/A.sdr/", "metadata.epub.lua"));
}

static void results(void)
{
	struct widget_state s = { 0 };
	CHECK(state_should_run(&s, TRIGGER_APPEAR));
	CHECK_INT(state_record(&s, RUN_OK, 11), OUTCOME_CHANGED);
	CHECK_INT(state_record(&s, RUN_OK, 11), OUTCOME_UNCHANGED); /* same output: no panel update */
	CHECK_INT(state_record(&s, RUN_OK, 12), OUTCOME_CHANGED);
	/* failures keep the last good output; the third in a row pauses */
	CHECK_INT(state_record(&s, RUN_TIME, 0), OUTCOME_FAILED);
	CHECK_INT(state_record(&s, RUN_ERROR, 0), OUTCOME_FAILED);
	CHECK(s.has_output && s.hash == 12);
	CHECK_INT(state_record(&s, RUN_MEMORY, 0), OUTCOME_PAUSED);
	CHECK(!state_should_run(&s, TRIGGER_APPEAR));
	CHECK(!state_should_run(&s, TRIGGER_INTERVAL));
	CHECK(!state_should_run(&s, TRIGGER_CHANGE));
	CHECK(state_should_run(&s, TRIGGER_SETTINGS));
	state_gallery_opened(&s);
	CHECK(state_should_run(&s, TRIGGER_APPEAR));
	/* a success in between resets the count */
	CHECK_INT(state_record(&s, RUN_TIMEOUT, 0), OUTCOME_FAILED);
	CHECK_INT(state_record(&s, RUN_TIMEOUT, 0), OUTCOME_FAILED);
	CHECK_INT(state_record(&s, RUN_OK, 12), OUTCOME_UNCHANGED);
	CHECK_INT(state_record(&s, RUN_BLOCKED, 0), OUTCOME_FAILED);
	CHECK_INT(s.fails, 1);
}

int main(void)
{
	grid();
	debounce();
	watching();
	results();
	return t_done("sched");
}
