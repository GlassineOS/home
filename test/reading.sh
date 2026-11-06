#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The built-in Continue reading widget against invented KOReader data, through the real sandbox.
set -eu
top=$(pwd)
run="$top/out/glassine-home-run"
fx="$top/test/fixtures/reading"
h="$top/out/reading-home"
fails=0 checks=0

reset_home() {
	rm -rf "$h"
	cp -R "$fx/home" "$h"
	# fixtures carry @HOME@ in place of the home directory
	find "$h" -type f -name '*.lua' | while read -r f; do
		sed -i "s|@HOME@|$h|g" "$f"
	done
}

check() { # name expected actual
	checks=$((checks + 1))
	if [ "$2" != "$3" ]; then
		fails=$((fails + 1))
		printf '%s: got\n  %s\nwant\n  %s\n' "$1" "$3" "$2" >&2
	fi
}

reset_home
want=$(sed "s|@HOME@|$h|g" "$fx/expected.json")
got=$(HOME="$h" "$run" "$top/widgets/reading")
check "last book" "$want" "$got"
again=$(HOME="$h" "$run" "$top/widgets/reading")
check "same bytes twice" "$got" "$again"

# what the run read (watched for on-change refresh)
files=$(HOME="$h" "$run" -v "$top/widgets/reading" 2>&1 >/dev/null | sed "s|$h|~|")
check "files read" "read: ~/.config/koreader/history.lua
read: ~/Books/The Lighthouse Keeper's Almanac.sdr/metadata.epub.lua" "$files"

# a book without metadata: the title comes from the file name
sed -i '4,7d' "$h/.config/koreader/history.lua"
got=$(HOME="$h" "$run" "$top/widgets/reading")
check "no metadata" "[{\"alt\":\"Cover of Notes on Small Gardens\",\"fit\":\"cover\",\"h\":96,\"side\":\"left\",\"src\":\"cover:$h/Books/Notes on Small Gardens.pdf\",\"t\":\"image\",\"w\":64},{\"t\":\"title\",\"text\":\"Notes on Small Gardens\"},{\"action\":{\"a\":\"open\",\"app\":\"koreader\",\"file\":\"$h/Books/Notes on Small Gardens.pdf\"},\"icon\":\"play_arrow\",\"label\":\"Continue\",\"t\":\"button\"},{\"t\":\"meta\",\"text\":\"Read\",\"time\":1699000000}]" "$got"

# a book outside the library: its metadata is not readable, the widget still shows it
reset_home
mkdir -p "$h/Elsewhere/Hidden.sdr"
cp "$h/Books/The Lighthouse Keeper's Almanac.sdr/metadata.epub.lua" "$h/Elsewhere/Hidden.sdr/metadata.epub.lua"
sed -i "s|$h/Books/The Lighthouse Keeper's Almanac.epub|$h/Elsewhere/Hidden.epub|" "$h/.config/koreader/history.lua"
got=$(HOME="$h" "$run" "$top/widgets/reading" | grep -o '"title","text":"[^"]*"')
check "outside the library" '"title","text":"Hidden"' "$got"

# nothing read yet, and no KOReader at all
printf 'return {}\n' > "$h/.config/koreader/history.lua"
want='[{"t":"title","text":"Continue reading"},{"soft":true,"t":"text","text":"Nothing read yet"}]'
check "empty history" "$want" "$(HOME="$h" "$run" "$top/widgets/reading")"
rm -rf "$h/.config/koreader"
check "no history" "$want" "$(HOME="$h" "$run" "$top/widgets/reading")"

printf '%-10s %d checks, %d failed\n' reading "$checks" "$fails"
[ "$fails" -eq 0 ]
