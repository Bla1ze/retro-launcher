#!/usr/bin/env bash
# Downloads per-system artwork into <app>/media/<system>/:
#   bezel.png    - system bezel from The Bezel Project (https://github.com/thebezelproject),
#                  fan-made artwork for personal use; it is not part of this repository.
#   console.*    - console photo by Evan Amos from Wikimedia Commons (public domain,
#                  except SNES and Lynx: CC BY-SA 3.0). Each photo's credit is kept in
#                  console.credit beside it; media/CREDITS.txt is rebuilt from those.
# Systems that already have a bezel / photo are skipped; --force fetches again.
# Usage: tools/fetch_media.sh <app folder> [--force]
set -u
APP=${1:?usage: fetch_media.sh <app folder> [--force]}
FORCE=0; [ "${2:-}" = "--force" ] && FORCE=1
D="$APP/media"; mkdir -p "$D"
UA="retro-launcher/1.0 (https://github.com/Bla1ze/retro-launcher)"

bezel() { # system repo
  local sys=$1 repo=$2 url
  [ $FORCE = 0 ] && [ -f "$D/$sys/bezel.png" ] && return
  url=$(curl -s "https://api.github.com/repos/thebezelproject/$repo/contents/retroarch/overlay" | python3 -c "
import json,sys
d=json.load(sys.stdin)
p=[x['download_url'] for x in d if x['name'].endswith('.png')] if isinstance(d,list) else []
print(p[0] if p else '')")
  mkdir -p "$D/$sys"
  [ -n "$url" ] && curl -sSfL -o "$D/$sys/bezel.png" "$url" && echo "$sys: bezel" || echo "$sys: no bezel"
}
photo() { # system commons-file...
  local sys=$1 f info u lic t e try; shift
  [ $FORCE = 0 ] && { [ -f "$D/$sys/console.png" ] || [ -f "$D/$sys/console.jpg" ]; } && return
  mkdir -p "$D/$sys"
  for f in "$@"; do
    info=""
    # Wikimedia rate-limits bursts (HTTP 429): pace the calls and retry after a wait.
    for try in 1 2 3 4; do
      sleep $((try * 3))
      info=$(curl -s -A "$UA" "https://commons.wikimedia.org/w/api.php?action=query&titles=File:$f&prop=imageinfo&iiprop=url|extmetadata&iiurlwidth=1400&format=json" | python3 -c "
import json,sys
try: pages=json.load(sys.stdin)['query']['pages'].values()
except Exception: sys.exit(1)
for p in pages:
    ii=p.get('imageinfo')
    if ii: print((ii[0].get('thumburl') or ii[0]['url']) + ' ' + ii[0].get('extmetadata',{}).get('LicenseShortName',{}).get('value','').replace(' ','_'))" 2>/dev/null) && break
    done
    [ -z "$info" ] && continue
    u=${info% *}; lic=${info##* }
    curl -sSfL -A "$UA" -o "$D/$sys/console.tmp" "$u" || continue
    t=$(file -b "$D/$sys/console.tmp" | cut -d' ' -f1); e=jpg; [ "$t" = PNG ] && e=png
    rm -f "$D/$sys"/console.png "$D/$sys"/console.jpg; mv "$D/$sys/console.tmp" "$D/$sys/console.$e"
    echo "  $sys/console.$e: \"$f\", ${lic//_/ }" > "$D/$sys/console.credit"; echo "$sys: photo ($f)"; return
  done
  echo "$sys: no photo"
}

bezel genesis bezelproject-MegaDrive;           photo genesis Sega-Genesis-Mod1-Set.jpg
bezel mastersystem bezelproject-MasterSystem;   photo mastersystem Sega-Master-System-Set.jpg
bezel gamegear bezelproject-GameGear;           photo gamegear Game-Gear-Handheld.jpg
bezel nes bezelproject-NES;                     photo nes NES-Console-Set.png
bezel snes bezelproject-SNES;                   photo snes SNES-Mod1-Console-Set.png
bezel atari2600 bezelproject-Atari2600;         photo atari2600 Atari-2600-Wood-4Sw-Set.png
bezel colecovision bezelproject-ColecoVision;   photo colecovision ColecoVision-wController-L.jpg
bezel gb bezelproject-GB;                       photo gb Game-Boy-FL.jpg
bezel gbc bezelproject-GBC;                     photo gbc Nintendo-Game-Boy-Color-FL.jpg
bezel gba bezelproject-GBA;                     photo gba Nintendo-Game-Boy-Advance-Purple-FL.jpg
bezel pce bezelproject-PCEngine;                photo pce TurboGrafx16-Console-Set.jpg
bezel lynx bezelproject-AtariLynx;              photo lynx Atari-Lynx-I-Handheld.jpg
bezel psx bezelproject-PSX;                     photo psx PSX-Console-wController.png
bezel dreamcast bezelproject-Dreamcast;         photo dreamcast Dreamcast-Console-Set.png

# Credits, rebuilt from every photo's own record.
{
  echo "Bezels: The Bezel Project, https://github.com/thebezelproject (personal use)."
  echo "Console photos: Evan Amos, Wikimedia Commons:"
  cat "$D"/*/console.credit 2>/dev/null
} > "$D/CREDITS.txt"
