#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The voices' text fonts for the renders, in out/fonts (fetched once, pinned like
# GlassineOS/shell's fonts/fetch-fonts.sh), and a fontconfig file that adds them to
# the system's: prints its path. White = IBM Plex Sans/Mono, Editorial = Source
# Serif 4, Stationery = EB Garamond (Alpine's font-eb-garamond). Offline: the
# renders use fallback fonts, and this says so on stderr.
set -eu
O=$(cd "$(dirname "$0")/.." && pwd)/out
F=$O/fonts
mkdir -p "$F"
GF=https://raw.githubusercontent.com/google/fonts/0b58fb370093f9a9f4ff785d94405710b79de67c/ofl
get() { # url file sha256
    [ -f "$F/$2" ] && return 0
    if command -v curl >/dev/null; then curl -sfL -o "$F/$2.part" "$1" || return 1
    else wget -q -O "$F/$2.part" "$1" || return 1; fi
    echo "$3  $F/$2.part" | sha256sum -c - >/dev/null || { rm -f "$F/$2.part"; echo "fonts: bad checksum: $2" >&2; return 1; }
    mv "$F/$2.part" "$F/$2"
}
ok=1
get "$GF/ibmplexsans/IBMPlexSans%5Bwdth%2Cwght%5D.ttf" IBMPlexSans-VF.ttf 3b031aa4216174205bd8471f88a49b91f093169e9e87bd5262242bc5967fe2e3 || ok=0
get "$GF/ibmplexmono/IBMPlexMono-Regular.ttf" IBMPlexMono-Regular.ttf 6a3412f058c7d8dfd9170c41e85ade48e5156ecb89356110ca57a0a27734af46 || ok=0
get "$GF/ibmplexmono/IBMPlexMono-Medium.ttf" IBMPlexMono-Medium.ttf a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755 || ok=0
get "$GF/ibmplexmono/IBMPlexMono-SemiBold.ttf" IBMPlexMono-SemiBold.ttf d3c38e55c78f5b0f28009fddba4834ec503278936a5986032424c9bd2d23aa46 || ok=0
get "$GF/sourceserif4/SourceSerif4%5Bopsz%2Cwght%5D.ttf" SourceSerif4-VF.ttf 97b2d4da6e3cb494b5a1e66ae176914d852ccabef49e0c02c0df25f3e39aca0b || ok=0
get "$GF/sourceserif4/SourceSerif4-Italic%5Bopsz%2Cwght%5D.ttf" SourceSerif4-Italic-VF.ttf 15fbc7e4679489a501998c3669272637a6646388ef7e4bd77eebb5bf967a1f42 || ok=0
[ "$ok" = 1 ] || echo "fonts: some of the voices' fonts could not be fetched (offline?): the renders use fallback fonts" >&2
cat > "$O/fonts.conf" <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <include ignore_missing="yes">/etc/fonts/fonts.conf</include>
  <dir>$F</dir>
  <cachedir>$O/fontcache</cachedir>
</fontconfig>
CONF
echo "$O/fonts.conf"
