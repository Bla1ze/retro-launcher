#!/usr/bin/env bash
# Builds a release zip of Retro Launcher from a staging app folder:
#   release/retro-launcher-v<version>.zip  ->  external/retro-launcher/...
# `external/` is the zip's only top-level item, so unzipping (macOS) gives just
# that folder, ready to copy to the root of the USB stick.
#
# Only an allow-list is copied, never the whole folder: the staging copy also
# holds a tester's games, saves, settings, logs and downloaded artwork, none of
# which may be published. The finished tree is then checked against a deny-list
# and the script fails if anything slipped in.
#
# What ships:
#   retro-launcher.elf / .xml / .png      the app
#   cores/*.so, *.db, arcade-genres.txt   emulators we build (sources in CORES.txt)
#   cores/CORES.txt, cores/README.txt
#   system/PPSSPP/                        PPSSPP's own support files
#   system/fbneo/hiscore.dat              FBNeo high-score data
#   licenses/                             GPL + font licenses
#   media/<system>/console.*              console photos (free licenses) + our drawings
#   media/<system>/console.credit, media/CREDITS.txt
# What never ships: roms/, saves/, data/, logs, BIOS or any .zip, box art,
# logos and bezels (each user downloads those in Settings > Download artwork).
#
# Usage: tools/make_release.sh <staging app folder> [output folder]
set -euo pipefail
SRC=${1:?usage: make_release.sh <staging app folder> [output folder]}
OUT=${2:-"$(cd "$(dirname "$0")/.." && pwd)/release"}
SRC=$(cd "$SRC" && pwd)

[ -f "$SRC/retro-launcher.elf" ] || { echo "no retro-launcher.elf in $SRC"; exit 1; }
VERSION=$(strings "$SRC/retro-launcher.elf" | sed -n 's/^Retro Launcher v\([0-9][0-9.]*\)$/\1/p' | head -1)
[ -n "$VERSION" ] || { echo "can't read the version from the ELF"; exit 1; }
[ -f "$SRC/cores/CORES.txt" ] || { echo "cores/CORES.txt missing (the GPL needs the core sources named)"; exit 1; }
[ -d "$SRC/licenses" ] || { echo "licenses/ missing (build the app first: CMake copies them)"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
APP="$TMP/external/retro-launcher"
mkdir -p "$APP/cores" "$APP/system" "$APP/licenses" "$APP/media"

cp "$SRC"/retro-launcher.elf "$SRC"/retro-launcher.xml "$SRC"/retro-launcher.png "$APP/"
for f in "$SRC"/cores/*.so "$SRC"/cores/*.db "$SRC"/cores/arcade-genres.txt "$SRC"/cores/CORES.txt "$SRC"/cores/README.txt; do
  [ -f "$f" ] && cp "$f" "$APP/cores/"
done
if [ -d "$SRC/system/PPSSPP" ]; then
  rsync -a --exclude '.DS_Store' --exclude 'SYSTEM' --exclude 'PSP' "$SRC/system/PPSSPP" "$APP/system/"
fi
if [ -f "$SRC/system/fbneo/hiscore.dat" ]; then mkdir -p "$APP/system/fbneo"; cp "$SRC/system/fbneo/hiscore.dat" "$APP/system/fbneo/"; fi
cp "$SRC"/licenses/* "$APP/licenses/"
for d in "$SRC"/media/*/; do
  sys=$(basename "$d")
  for f in "$d"console.png "$d"console.jpg "$d"console.credit; do
    [ -f "$f" ] && { mkdir -p "$APP/media/$sys"; cp "$f" "$APP/media/$sys/"; }
  done
done
# The credits list, without the bezel line (no bezels ship).
{
  echo "Console pictures shipped with Retro Launcher. Photos from Wikimedia Commons, shown with the white"
  echo "background removed, cropped and resized. CC BY-SA 3.0: https://creativecommons.org/licenses/by-sa/3.0/"
  cat "$APP"/media/*/console.credit
} > "$APP/media/CREDITS.txt"
cat > "$APP/INSTALL.txt" <<EOF
Retro Launcher v$VERSION

Copy the "external" folder this file is in to the root of your USB stick
(merge it with an existing one), plug the stick into the cabinet and start
Retro Launcher from the External Applications menu. On Windows, "Extract All"
suggests a folder named after the zip: extract to the stick itself instead, or
copy the "external" folder from inside it.

Later versions install themselves: Settings > Updates.

Then add your own games to external/retro-launcher/roms/<system>/ (on a
computer, or with Settings > Network transfer), and fetch box art, logos and
bezels with Settings > Download artwork.

Retro Launcher ships no games and no BIOS files.
Source, credits and licenses: https://github.com/Bla1ze/retro-launcher
EOF

# Deny-list: anything personal, copyrighted or downloaded must not be in here.
BAD=$(cd "$TMP" && find . -type f \( \
    -path '*/roms/*' -o -path '*/saves/*' -o -path '*/data/*' -o -path '*/trash/*' \
    -o -name '*.log' -o -name 'output.txt' -o -name '*.zip' -o -name '*.7z' -o -name '*.state' -o -name '*.srm' \
    -o \( \( -name '*.bin' -o -name '*.rom' -o -name '*.chd' -o -name '*.iso' -o -name '*.cso' \) \
           -not -path '*/system/PPSSPP/*' \) \
    -o -path '*/Named_*' -o -path '*/boxart/*' -o -path '*/bezels/*' -o -name 'bezel.png' \
    -o -path '*/.art-index/*' -o -path '*/.boxart-trees/*' -o -name '.DS_Store' \) -print)
if [ -n "$BAD" ]; then
  echo "refusing to package; these must not ship:"; echo "$BAD"; exit 1
fi

mkdir -p "$OUT"
ZIP="$OUT/retro-launcher-v$VERSION.zip"
rm -f "$ZIP"
(cd "$TMP" && zip -r -X -q "$ZIP" external)
echo "$ZIP"
echo "  $(cd "$TMP" && find . -type f | wc -l | tr -d ' ') files, $(du -h "$ZIP" | cut -f1) zipped"
echo "  cores: $(ls "$APP/cores" | grep -c '\.so$'), console pictures: $(ls "$APP"/media/*/console.* 2>/dev/null | grep -vc credit)"
