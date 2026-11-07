#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Prints the path of a GlassineOS/ui checkout (the voices and the drawing,
# pkg-config glassine-ui), as GlassineOS/shell's test/ui.sh does:
#   $GLASSINE_UI if set, else ../ui beside this repository, else a clone of
#   GlassineOS/ui at $GLASSINE_UI_REF (default: main) in out/ui-src.
set -eu
R=$(cd "$(dirname "$0")/.." && pwd)
if [ -n "${GLASSINE_UI:-}" ]; then
    [ -f "$GLASSINE_UI/src/glassine-ui.h" ] || { echo "GLASSINE_UI=$GLASSINE_UI: not a GlassineOS/ui checkout" >&2; exit 1; }
    cd "$GLASSINE_UI" && pwd
elif [ -f "$R/../ui/src/glassine-ui.h" ]; then
    cd "$R/../ui" && pwd
else
    C=$R/out/ui-src
    if [ ! -d "$C/.git" ]; then
        mkdir -p "$R/out"
        git clone -q https://github.com/GlassineOS/ui "$C" >&2
    fi
    ref=${GLASSINE_UI_REF:-main}
    git -C "$C" fetch -q --tags origin >&2
    git -C "$C" checkout -q --detach "origin/$ref" 2>/dev/null || git -C "$C" checkout -q --detach "$ref" >&2
    echo "$C"
fi
