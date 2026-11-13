#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# glassine-home in a headless sway, floating as the session shows it (border none, resize
# set, move absolute): the surface follows every size sway gives it, in both orientations,
# smaller and larger, and a size given while it sits in the scratchpad once it is shown.
# Checked twice: the last buffer Home committed (WAYLAND_DEBUG) is the container's size
# in buffer px, and a screenshot (grim) shows Home's paper, not the black output behind it,
# in the container's far corner. Without sway or grim: skipped.
set -eu
top=$(pwd)
home="$top/out/glassine-home"
if ! command -v sway >/dev/null || ! command -v swaymsg >/dev/null || ! command -v grim >/dev/null; then
	echo "resize.sh: skipped (no sway, swaymsg or grim)"
	exit 0
fi
[ -x "$home" ] || { echo "resize.sh: no $home" >&2; exit 1; }
t="$top/out/resize-test"
rm -rf "$t"
mkdir -p -m 700 "$t/rt"
mkdir -p "$t/home"
export XDG_RUNTIME_DIR="$t/rt" HOME="$t/home"
unset WAYLAND_DISPLAY SWAYSOCK DISPLAY
cp "$(command -v sway)" "$t/sway" # the package's sway has cap_sys_nice: no exec in a container
# the panel's size in device px, scale 2: 702 x 936 logical
printf 'output HEADLESS-1 mode 1404x1872 scale 2\noutput HEADLESS-1 bg #000000 solid_color\nfor_window [app_id="glassine-home"] floating enable, border none\n' >"$t/sway.conf"
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
SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock) || { echo "resize.sh: sway did not start" >&2; cat "$t/sway.log" >&2; exit 1; }
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

# "W H": the container's size, logical px (empty while Home is hidden or not mapped)
rect() {
	swaymsg -t get_tree -r | python3 -c '
import json, sys
def walk(n):
    if n.get("app_id") == "glassine-home" and n.get("visible"):
        print(n["rect"]["width"], n["rect"]["height"], n["rect"]["x"], n["rect"]["y"])
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c)
walk(json.load(sys.stdin))'
}

# "W H": the size of the buffer Home attached last, from its WAYLAND_DEBUG log
committed() {
	python3 -c '
import re, sys
size, last = {}, None
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"create_buffer\(new id wl_buffer#(\d+), 0, (\d+), (\d+),", line)
    if m:
        size[m.group(1)] = m.group(2) + " " + m.group(3)
    m = re.search(r"-> wl_surface#\d+\.attach\(wl_buffer#(\d+),", line)
    if m:
        last = size.get(m.group(1))
print(last or "")' "$t/home.log"
}

# Home's paper in the container's far corner: "paper" or "black" (a screenshot, PPM)
corner() { # W H X Y, logical px
	grim -t ppm "$t/shot.ppm"
	python3 -c '
import sys
w, h, x, y = map(int, sys.argv[2:6])
d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3)          # P6, size, maxval, pixels (grim writes no comments)
pw, ph = map(int, parts[1].split())
px = parts[3]
black = 0
for yy in range((y + h) * 2 - 8, (y + h) * 2 - 2):
    for xx in range((x + w) * 2 - 8, (x + w) * 2 - 2):
        o = (yy * pw + xx) * 3
        black += px[o:o + 3] == b"\0\0\0"
print("black" if black > 18 else "paper")' "$t/shot.ppm" "$@"
}

# waits up to 5 s for Home's last buffer to be the container's size in buffer px (scale 2)
settle() {
	for _ in $(seq 1 50); do
		r=$(rect)
		if [ -n "$r" ]; then
			set -- $r
			[ "$(committed)" = "$(($1 * 2)) $(($2 * 2))" ] && return 0
		fi
		sleep 0.1
	done
	return 0
}

# step NAME W H: the container's size and what Home committed and shows
step() {
	settle
	r=$(rect)
	check "$1: container" "$2 $3" "$(echo "$r" | cut -d' ' -f1-2)"
	check "$1: committed buffer (px)" "$(($2 * 2)) $(($3 * 2))" "$(committed)"
	# shellcheck disable=SC2086
	[ -z "$r" ] || check "$1: Home's paper in the far corner" "paper" "$(corner $r)"
}

sway_() { swaymsg -q "$@"; }
for _ in $(seq 1 100); do [ -n "$(rect)" ] && break; sleep 0.1; done
show() { sway_ "[app_id=glassine-home] border none; resize set width $1 px height $2 px; move absolute position $3 px $4 px"; }

show 702 884 0 52
step "portrait" 702 884
sway_ "output HEADLESS-1 transform 90"
show 936 702 0 0
step "turned to landscape while shown (larger, wider)" 936 702
show 936 530 0 172
step "landscape, smaller" 936 530
sway_ "output HEADLESS-1 transform normal"
show 702 884 0 52
step "back to portrait while shown" 702 884
show 702 584 0 352
step "portrait, smaller" 702 584
show 702 884 0 52
step "portrait, larger" 702 884
# hidden: sized and the screen turned while in the scratchpad, then shown
sway_ "[app_id=glassine-home] move scratchpad"
sway_ "output HEADLESS-1 transform 90"
show 936 530 0 172 || : # sway refuses the move (no workspace) but takes the size
sleep 0.5
sway_ "[app_id=glassine-home] scratchpad show"
show 936 530 0 172
step "sized while hidden (landscape), then shown" 936 530
sway_ "[app_id=glassine-home] move scratchpad"
sway_ "output HEADLESS-1 transform normal"
show 702 584 0 352 || :
sleep 0.5
sway_ "[app_id=glassine-home] scratchpad show"
show 702 584 0 352
step "sized while hidden (portrait), then shown" 702 584

kill -0 "$home_pid" 2>/dev/null || check "Home still runs" "yes" "no"
if [ "$fails" -gt 0 ]; then
	echo "resize.sh: $fails of $checks checks failed; logs in $t" >&2
	exit 1
fi
echo "resize.sh: $checks checks passed"
