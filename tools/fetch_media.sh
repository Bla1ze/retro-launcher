#!/usr/bin/env bash
# Downloads per-system artwork into <app>/media/<system>/:
#   bezel.png    - system bezel from The Bezel Project (https://github.com/thebezelproject),
#                  fan-made artwork for personal use; it is not part of this repository.
#   console.*    - console photo by Evan Amos from Wikimedia Commons (public domain,
#                  except SNES and Lynx: CC BY-SA 3.0). Credits go to media/CREDITS.txt.
# Usage: tools/fetch_media.sh <app folder>
set -u
APP=${1:?usage: fetch_media.sh <app folder>}
D="$APP/media"; mkdir -p "$D"
echo "Bezels: The Bezel Project, https://github.com/thebezelproject (personal use)." > "$D/CREDITS.txt"
echo "Console photos: Evan Amos, Wikimedia Commons:" >> "$D/CREDITS.txt"

bezel() { # system repo
  local sys=$1 repo=$2 url
  url=$(curl -s "https://api.github.com/repos/thebezelproject/$repo/contents/retroarch/overlay" | python3 -c "
import json,sys
d=json.load(sys.stdin)
p=[x['download_url'] for x in d if x['name'].endswith('.png')] if isinstance(d,list) else []
print(p[0] if p else '')")
  mkdir -p "$D/$sys"
  [ -n "$url" ] && curl -sSfL -o "$D/$sys/bezel.png" "$url" && echo "$sys: bezel" || echo "$sys: no bezel"
}
photo() { # system commons-file...
  local sys=$1 f info u lic t e; shift
  for f in "$@"; do
    info=$(curl -s "https://commons.wikimedia.org/w/api.php?action=query&titles=File:$f&prop=imageinfo&iiprop=url|extmetadata&iiurlwidth=1400&format=json" | python3 -c "
import json,sys
for p in json.load(sys.stdin)['query']['pages'].values():
    ii=p.get('imageinfo')
    if ii: print((ii[0].get('thumburl') or ii[0]['url']) + ' ' + ii[0].get('extmetadata',{}).get('LicenseShortName',{}).get('value','').replace(' ','_'))")
    [ -z "$info" ] && continue
    u=${info% *}; lic=${info##* }
    curl -sSfL -A "retro-launcher (personal use)" -o "$D/$sys/console.tmp" "$u" || continue
    t=$(file -b "$D/$sys/console.tmp" | cut -d' ' -f1); e=jpg; [ "$t" = PNG ] && e=png
    rm -f "$D/$sys"/console.png "$D/$sys"/console.jpg; mv "$D/$sys/console.tmp" "$D/$sys/console.$e"
    echo "  $sys/console.$e: \"$f\", ${lic//_/ }" >> "$D/CREDITS.txt"; echo "$sys: photo ($f)"; return
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
