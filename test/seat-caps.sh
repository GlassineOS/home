#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# glassine-home in a headless sway while the seat loses a device and gets it back
# (the touchscreen across a suspend does this): each wlrctl call adds a virtual
# pointer and removes it when it exits. A lost device's object stays inert, so Home
# must release it and ask for a new one when the device comes back. Checked in
# Home's WAYLAND_DEBUG log: a get_pointer per arrival, a release per departure.
# Without sway or wlrctl: skipped.
set -eu
top=$(pwd)
home="$top/out/glassine-home"
if ! command -v sway >/dev/null || ! command -v wlrctl >/dev/null; then
	echo "seat-caps.sh: skipped (no sway or wlrctl)"
	exit 0
fi
[ -x "$home" ] || { echo "seat-caps.sh: no $home" >&2; exit 1; }
t="$top/out/seat-caps-test"
rm -rf "$t"
mkdir -p -m 700 "$t/rt"
mkdir -p "$t/home"
export XDG_RUNTIME_DIR="$t/rt" HOME="$t/home"
unset WAYLAND_DISPLAY SWAYSOCK DISPLAY
cp "$(command -v sway)" "$t/sway" # the package's sway has cap_sys_nice: no exec in a container
printf 'output HEADLESS-1 mode 1404x1872 scale 2\n' >"$t/sway.conf"
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
ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock >/dev/null 2>&1 || { echo "seat-caps.sh: sway did not start" >&2; cat "$t/sway.log" >&2; exit 1; }
WAYLAND_DISPLAY=$(cd "$XDG_RUNTIME_DIR" && ls wayland-* | grep -v lock | head -1)
export WAYLAND_DISPLAY

GLASSINE_HOME_WIDGETS="$top/widgets" GLASSINE_HOME_NATIVE="$top/native" GLASSINE_HOME_SYSTEM_WIDGETS="$t/none" \
	GLASSINE_HOME_LAUNCH=true GLASSINE_HOME_HIDE=true GLASSINE_HOME_EINK_CONF="$t/none" \
	WAYLAND_DEBUG=1 "$home" >"$t/home.log" 2>&1 &
home_pid=$!
sleep 2

count() { grep -c -E "$1" "$t/home.log" || :; }
fails=0
check() { # name expected actual
	if [ "$2" != "$3" ]; then
		fails=$((fails + 1))
		printf 'FAIL %s: got %s, want %s\n' "$1" "$3" "$2" >&2
	fi
}
check "no pointer at start" 0 "$(count '\.get_pointer\(')"
for round in 1 2 3; do
	wlrctl pointer move 1 1
	sleep 0.5
	check "round $round: a pointer asked for" "$round" "$(count '\.get_pointer\(')"
	check "round $round: the lost pointer released" "$round" "$(count 'wl_pointer#[0-9]+\.release\(')"
done
if [ "$fails" -gt 0 ]; then
	echo "seat-caps.sh: $fails failed (log: $t/home.log)" >&2
	exit 1
fi
echo "seat-caps.sh: a lost pointer is released and a new one asked for, 3 rounds"
