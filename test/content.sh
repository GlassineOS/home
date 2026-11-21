#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# glassine-home in a headless sway that offers the ink protocol's version 4 (sway-glassine,
# or $GLASSINE_TEST_SWAY): Home says a tap's answer is a page, on the commit that shows it.
# Taps over the whole window (test/vpointer.c: one virtual pointer); in Home's WAYLAND_DEBUG log every set_content(1)
# must follow a button release with no commit of Home's surface between them (the first
# paint after the tap), be followed by that commit, and no release may give two. At least
# one tap must give a page; a run with GLASSINE_HOME_CONTENT=off says nothing.
# Without such a sway or the wlr-protocols XML: skipped.
set -eu
top=$(pwd)
home="$top/out/glassine-home"
sway=${GLASSINE_TEST_SWAY:-$(command -v sway-glassine || :)}
vp_xml=${WLR_PROTOCOLS:-/usr/share/wlr-protocols}/unstable/wlr-virtual-pointer-unstable-v1.xml
if [ -z "$sway" ] || [ ! -f "$vp_xml" ]; then
	echo "content.sh: skipped (no sway-glassine or no wlr-protocols)"
	exit 0
fi
[ -x "$home" ] || { echo "content.sh: no $home" >&2; exit 1; }
t="$top/out/content-test"
rm -rf "$t"
mkdir -p -m 700 "$t/rt"
mkdir -p "$t/home"
wayland-scanner client-header "$vp_xml" "$t/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code "$vp_xml" "$t/wlr-virtual-pointer-unstable-v1-protocol.c"
${CC:-cc} -O1 -I"$t" -o "$t/vpointer" test/vpointer.c "$t/wlr-virtual-pointer-unstable-v1-protocol.c" \
	$(pkg-config --cflags --libs wayland-client)
export XDG_RUNTIME_DIR="$t/rt" HOME="$t/home"
unset WAYLAND_DISPLAY SWAYSOCK DISPLAY
cp "$sway" "$t/sway" # the package's sway has cap_sys_nice: no exec in a container
printf 'output HEADLESS-1 mode 1404x1872 scale 2\ndefault_border none\n' >"$t/sway.conf"
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 "$t/sway" -c "$t/sway.conf" >"$t/sway.log" 2>&1 &
sp=$!
home_pid=
cleanup() {
	[ -z "$home_pid" ] || kill "$home_pid" 2>/dev/null || :
	kill "$sp" 2>/dev/null || :
	wait 2>/dev/null || :
}
trap cleanup EXIT
for _ in $(seq 1 100); do
	ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock >/dev/null 2>&1 && break
	sleep 0.1
done
ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock >/dev/null 2>&1 || { echo "content.sh: sway did not start" >&2; cat "$t/sway.log" >&2; exit 1; }
WAYLAND_DISPLAY=$(cd "$XDG_RUNTIME_DIR" && ls wayland-* | grep -v lock | head -1)
export WAYLAND_DISPLAY

fails=0
run() { # LOG [ENV=VALUE]: Home, tapped on a grid over the window
	log=$1
	shift
	env GLASSINE_HOME_WIDGETS="$top/widgets" GLASSINE_HOME_NATIVE="$top/native" GLASSINE_HOME_SYSTEM_WIDGETS="$t/none" \
		GLASSINE_HOME_LAUNCH=true GLASSINE_HOME_HIDE=true GLASSINE_HOME_EINK_CONF="$t/none" \
		WAYLAND_DEBUG=1 "$@" "$home" >"$log" 2>&1 &
	home_pid=$!
	sleep 2
	pts=
	for y in 60 200 340 480 620 760 900; do
		for x in 80 350 620; do
			pts="$pts $x,$y"
		done
	done
	# the output is 702x936 logical px (1404x1872 at scale 2)
	# shellcheck disable=SC2086
	"$t/vpointer" 702 936 1000 400 $pts
	sleep 1
	kill "$home_pid" 2>/dev/null || :
	wait "$home_pid" 2>/dev/null || :
	home_pid=
}

run "$t/home.log"
if ! grep -qE 'glassine_ink_manager_v1", [4-9]' "$t/home.log"; then
	echo "content.sh: skipped (the sway offers no glassine_ink_manager_v1 version 4)"
	exit 0
fi
# the requests and events that matter, in order: R a release, C a commit of Home's
# surface, P a page, S another set_content
awk '
	/get_ink_surface\(/ { if (match($0, /wl_surface#[0-9]+/)) surf = substr($0, RSTART, RLENGTH) }
	/wl_pointer#[0-9]+\.button\(.*, 0\)$/ { print "R"; next }
	/set_content\(1\)/ { print "P"; next }
	/set_content\(/ { print "S"; next }
	surf != "" && index($0, surf ".commit()") { print "C" }
' "$t/home.log" | tr -d '\n' >"$t/seq"
seq=$(cat "$t/seq")
pages=$(tr -cd 'P' <"$t/seq" | wc -c)
taps=$(tr -cd 'R' <"$t/seq" | wc -c)
bad() {
	fails=$((fails + 1))
	printf 'FAIL %s\n' "$1" >&2
}
[ "$taps" -ge 10 ] || bad "taps reached Home: $taps"
[ "$pages" -ge 1 ] || bad "no tap gave a page ($taps taps)"
# a page right after a release (no commit between), and its commit right after it
echo "$seq" | grep -qE '(^|C)P|PP|P(R|S)|P$' && bad "a page not on the first commit after a tap: $seq"
echo "$seq" | grep -q 'S' && bad "a kind other than page: $seq"
# no release gives two pages
echo "$seq" | grep -qE 'P[^R]*P' && bad "two pages for one tap: $seq"

run "$t/off.log" GLASSINE_HOME_CONTENT=off
grep -qE 'get_ink_surface|set_content' "$t/off.log" && bad "GLASSINE_HOME_CONTENT=off still said something"

if [ "$fails" -gt 0 ]; then
	echo "content.sh: $fails failed (logs: $t)" >&2
	exit 1
fi
echo "content.sh: $pages of $taps taps answered by a page, each on the first commit after it; off says nothing"
