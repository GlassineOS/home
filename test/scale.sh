#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# glassine-home in a headless sway at the scales the session gives an output for the
# interface sizes (GlassineOS/shell spec 012): Home takes the output's factor as a
# fractional scale and commits a buffer of the window's logical size times it, shown
# through a viewport at the logical size; at a whole scale it commits at that buffer
# scale and sets no viewport size, as before. The scale changes while Home is shown,
# and Home moves to a second output of another scale. Checked in Home's WAYLAND_DEBUG
# log. Without sway: skipped.
set -eu
top=$(pwd)
home="$top/out/glassine-home"
if ! command -v sway >/dev/null || ! command -v swaymsg >/dev/null; then
	echo "scale.sh: skipped (no sway)"
	exit 0
fi
[ -x "$home" ] || { echo "scale.sh: no $home" >&2; exit 1; }
t="$top/out/scale-test"
rm -rf "$t"
mkdir -p -m 700 "$t/rt"
mkdir -p "$t/home"
export XDG_RUNTIME_DIR="$t/rt" HOME="$t/home"
unset WAYLAND_DISPLAY SWAYSOCK DISPLAY
cp "$(command -v sway)" "$t/sway" # the package's sway has cap_sys_nice: no exec in a container
printf 'output HEADLESS-1 mode 1404x1872 scale 2\nfor_window [app_id="glassine-home"] floating enable, border none\n' >"$t/sway.conf"
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
SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock) || { echo "scale.sh: sway did not start" >&2; cat "$t/sway.log" >&2; exit 1; }
export SWAYSOCK
WAYLAND_DISPLAY=$(cd "$XDG_RUNTIME_DIR" && ls wayland-* | grep -v lock | head -1)
export WAYLAND_DISPLAY

GLASSINE_HOME_WIDGETS="$top/widgets" GLASSINE_HOME_NATIVE="$top/native" GLASSINE_HOME_SYSTEM_WIDGETS="$t/none" \
	GLASSINE_HOME_LAUNCH=true GLASSINE_HOME_HIDE=true GLASSINE_HOME_EINK_CONF="$t/none" \
	WAYLAND_DEBUG=1 "$home" >"$t/home.log" 2>&1 &
home_pid=$!

fails=0 checks=0
check() { # name expected actual
	checks=$((checks + 1))
	if [ "$2" != "$3" ]; then
		fails=$((fails + 1))
		printf 'FAIL %s: got\n  %s\nwant\n  %s\n' "$1" "$3" "$2" >&2
	fi
}

# "W H": the container's size, logical px
rect() {
	swaymsg -t get_tree -r | python3 -c '
import json, sys
def walk(n):
    if n.get("app_id") == "glassine-home" and n.get("visible"):
        print(n["rect"]["width"], n["rect"]["height"])
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c)
walk(json.load(sys.stdin))'
}

# "W H SCALE DEST": what Home's last commit carried: the buffer's size, the buffer scale
# and the viewport's destination ("-1 -1": none) set before it
committed() {
	python3 -c '
import re, sys
size, buf, sc, dest = {}, "", "1", "-1 -1"
last = ""
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"create_buffer\(new id wl_buffer#(\d+), 0, (\d+), (\d+),", line)
    if m:
        size[m.group(1)] = m.group(2) + " " + m.group(3)
    m = re.search(r"-> wl_surface#\d+\.attach\(wl_buffer#(\d+),", line)
    if m:
        buf = size.get(m.group(1), "")
    m = re.search(r"-> wl_surface#\d+\.set_buffer_scale\((\d+)\)", line)
    if m:
        sc = m.group(1)
    m = re.search(r"-> wp_viewport#\d+\.set_destination\((-?\d+), (-?\d+)\)", line)
    if m:
        dest = m.group(1) + " " + m.group(2)
    if re.search(r"-> wl_surface#\d+\.commit\(\)", line) and buf:
        last = "%s %s %s" % (buf, sc, dest)
print(last)' "$t/home.log"
}

# want W H SCALE: what Home should commit for a container of W x H at SCALE
want() {
	python3 -c '
import sys
w, h, s = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
if abs(s - round(s)) < 1e-9:
    print(w * round(s), h * round(s), round(s), -1, -1)
else:
    print(round(w * s), round(h * s), 1, w, h)' "$@"
}

settle() { # SCALE
	for _ in $(seq 1 50); do
		r=$(rect)
		if [ -n "$r" ]; then
			# shellcheck disable=SC2086
			[ "$(committed)" = "$(want $r "$1")" ] && return 0
		fi
		sleep 0.1
	done
	return 0
}

step() { # NAME SCALE
	settle "$2"
	r=$(rect)
	# shellcheck disable=SC2086
	check "$1: committed (buffer px, buffer scale, viewport)" "$(want $r "$2")" "$(committed)"
}

sway_() { swaymsg -q "$@"; }
for _ in $(seq 1 100); do [ -n "$(rect)" ] && break; sleep 0.1; done
show() { sway_ "[app_id=glassine-home] border none; resize set width $1 px height $2 px; move absolute position 0 px 0 px"; }

show 702 884
step "scale 2" 2
for s in 1.6 1.8 2.4 1.5 2; do
	sway_ "output HEADLESS-1 scale $s"
	l=$(python3 -c "import sys; print(int(1404 / $s), int(1768 / $s))")
	# shellcheck disable=SC2086
	show $l
	step "scale $s" "$s"
done

# a second output at another scale: Home moved there takes its factor, and back
sway_ create_output
sway_ "output HEADLESS-2 mode 1280x800 scale 1.25 position 1000 0"
sway_ "[app_id=glassine-home] move to output HEADLESS-2"
sway_ "[app_id=glassine-home] resize set width 800 px height 600 px" || :
sway_ "[app_id=glassine-home] move position center" || :
step "on a second output at 1.25" 1.25
sway_ "[app_id=glassine-home] move to output HEADLESS-1"
sway_ "[app_id=glassine-home] resize set width 702 px height 884 px" || :
step "back on the first, at 2" 2

kill -0 "$home_pid" 2>/dev/null || check "Home still runs" "yes" "no"
if [ "$fails" -gt 0 ]; then
	echo "scale.sh: $fails of $checks checks failed; logs in $t" >&2
	exit 1
fi
echo "scale.sh: $checks checks passed"
