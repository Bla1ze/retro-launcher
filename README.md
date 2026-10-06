# Retro Launcher

**Website: [bla1ze.github.io/retro-launcher](https://bla1ze.github.io/retro-launcher/)**

A console game launcher for **AtGames Legends Pinball** cabinets, with its own
built-in emulator front-end. Browse and search your games on the playfield,
play them on the backglass, and see box art, controls and the console itself on
the other screens.

It runs as a BYOG **External Application** (no firmware changes, no root) and
plays games with [libretro](https://www.libretro.com/) emulator cores: the ones
already in the cabinet's firmware, or better ones built for it (see
[Cores](#cores)).

> Retro Launcher is a fan project. It is not affiliated with or endorsed by
> AtGames, Nintendo, Sega, NEC, Atari or Coleco. It ships **no games, BIOS files
> or copyrighted artwork**. Use it with games you own.

## Features

- **Menu on the playfield**: Neon-styled systems and game lists, search across
  every system with an on-screen keyboard, Recently played, Favorites, A–Z
  letter jumps on the flippers, and an in-app Settings screen.
- **Arcade**: FBNeo and MAME 2003-Plus, chosen per game by checking each zip
  against both emulators' ROM lists; real game names; vertical games full-size
  on the portrait playfield.
- **Console photos in the systems list** (from `media/<system>/console.*`, cut out
  of their white background), with a drawn gamepad for any system without one.
- **Choose the screen per game**: backglass or playfield (Home on a game),
  with a default in Settings. The screens a game isn't using show artwork.
- **Backglass and DMD while browsing**: the highlighted game's box art over a
  blurred copy of itself on the backglass, and its title on the DMD. Games
  without art get a generated cover.
- **Games on the backglass** (or the playfield), with the picture shape correct
  per system. Bezels (The Bezel Project format) or an ambient glow taken from the
  game's own colors fill the sides. Smooth, sharp or pixel-perfect scaling, and
  optional CRT scanlines. Blank edge columns (Master System, NES) are cropped so the
  picture is centered.
- **While playing**: a "Now playing" card on the playfield with the cover and the
  game's controls, and a photo of the console on the DMD.
- **Button layouts** per system and per game: presets or press-to-assign, from
  Home > Controls.
- **Pause menu**: hold Start or press Home for Resume, Save state, Load state,
  Reset, Core options, Change disc (multi-disc games) and Quit. Core options lists every setting the running
  emulator offers and applies changes straight away. Quitting saves your place, and the next launch offers
  "Continue where you left off".
- **Solid playback**: vsync-locked pacing with dynamic audio rate control for
  60 Hz games, audio-clock pacing for 50 Hz (PAL) games, a watchdog for a stuck
  core, and a crash guard that returns to the menu instead of the firmware.

## Systems

| System | Folder | Core | Source |
|---|---|---|---|
| Genesis / Mega Drive | `roms/genesis` | Genesis Plus GX | firmware |
| Master System | `roms/mastersystem` | Genesis Plus GX | firmware |
| Game Gear | `roms/gamegear` | Genesis Plus GX | firmware |
| NES | `roms/nes` | FCEUmm (else QuickNES) | `cores/` (else firmware) |
| Super Nintendo | `roms/snes` | Snes9x (else SNES Faust) | `cores/` (else firmware) |
| Atari 2600 | `roms/atari2600` | Stella | firmware |
| ColecoVision | `roms/colecovision` | Gearcoleco with a BIOS in `system/`, else the firmware's (built-in BIOS) | `cores/` |
| Game Boy | `roms/gb` | Gambatte | `cores/` |
| Game Boy Color | `roms/gbc` | Gambatte | `cores/` |
| Game Boy Advance | `roms/gba` | gpSP | `cores/` |
| PC Engine / TurboGrafx-16 | `roms/pce` | Beetle PCE Fast | `cores/` |
| Atari Lynx | `roms/lynx` | Handy | `cores/` |
| PlayStation | `roms/psx` | PCSX ReARMed | `cores/` |
| Dreamcast | `roms/dreamcast` | Flycast (GPU) | `cores/` |
| PSP | `roms/psp` | PPSSPP (GPU) | `cores/` |
| Nintendo 64 | `roms/n64` | Mupen64Plus-Next (GPU) | `cores/` |
| Saturn | `roms/saturn` | YabaSanshiro (GPU) | `cores/` |
| NAOMI | `roms/naomi` | Flycast (GPU), checked per set | `cores/` |
| Atomiswave | `roms/atomiswave` | Flycast (GPU), checked per set | `cores/` |
| Neo Geo | `roms/neogeo` | FBNeo, checked per set | `cores/` |
| Arcade | `roms/arcade` | FBNeo or MAME 2003-Plus, picked per game | `cores/` |

ROMs can be plain files or `.zip`.

### PlayStation

Put games in `roms/psx/` as `.chd` (smallest, recommended), `.pbp`, or `.cue` with
its `.bin` tracks beside it. Only the `.cue` is listed, and an `.m3u` playlist
hides the discs it names. Don't zip disc images. PCSX ReARMed has a built-in
BIOS; a real one (`scph5501.bin` for USA games) in `system/` runs more games.

### Dreamcast

Put games in `roms/dreamcast/` as `.chd` (recommended), `.gdi` with its track
files beside it, or `.cdi`. Flycast draws with the GPU (OpenGL ES 3): it gets its
own GL context and each frame is read back into the normal picture path. Its
built-in BIOS replacement runs official discs; homebrew and conversions (such as
the Atomiswave ports of Dolphin Blue and Metal Slug 6) hang on it and need a real
`dc_boot.bin` / `dc_flash.bin` in `system/dc/`. A game in its own folder
(`Game/disc.gdi`) is listed under the folder's name.

### PSP

Put games in `roms/psp/` as `.iso`, `.cso` or `.chd`. No BIOS is needed, but
PPSSPP's own files (its `assets` folder: fonts, shaders) must be in
`system/PPSSPP/`. The joystick drives the analog nub (the D-pad at full tilt
when the stick reports as one).

### Nintendo 64 and Saturn

N64 games (`.z64`, `.n64`, `.v64`) go in `roms/n64/`; the C buttons are on their
own buttons (the core's independent C-button layout is the default here, since
the cabinet has no right stick). Saturn discs (`.chd`, `.cue`) go in
`roms/saturn/`; YabaSanshiro has a built-in BIOS replacement, and a real
`saturn_bios.bin` in `system/` runs more games.

### ColecoVision

Without a BIOS file in `system/`, games run on the cabinet's own ColecoVision
emulator, which has the BIOS built in. With `colecovision.rom` there, Gearcoleco
runs them instead. The keypad on the cabinet's own emulator:
- **Start** presses keypad 1, which starts most games at skill 1.
- **Rewind** presses keypad \*.
- **Y** opens an on-screen keypad for the other keys: move with the D-pad, press with B.

**Auto-start** skips the "select game" screen. After the BIOS title, the player
presses a keypad key for you. It waits for the title, then the select screen,
and stops once the game is moving. Pressing any button, or 45 seconds passing,
also stops it.
- The key is keypad 1 unless you change it.
- Change it per game with **Home > Start with**: Off, or 1 to 8. In most games,
  1 to 4 are the skill levels for one player and 5 to 8 for two.

### Marquees (game logos on the DMD)

With `tools/prefill_boxart.py <app> --logos`, libretro's game logos go to
`media/<system>/Named_Logos/` (PNG, transparent). The DMD then shows the
highlighted game's logo while browsing and while playing, like a marquee, with
the system's name under it. Arcade, NAOMI and Atomiswave use the MAME set;
systems without logos (Game Boy Color, Lynx, PSP) keep the title / console photo.

### GPU picture quality

GPU systems render above their original resolution by default: Dreamcast,
NAOMI and N64 at 2x (1280x960), PSP at 2x (960x544; 3x was too slow for
heavier games like GTA: Liberty City Stories). Saturn stays at its
original resolution (YabaSanshiro has no headroom for more in heavy games), with
auto-frameskip on, so busy scenes drop a drawn frame rather than the sound. Each
can be changed in the pause menu's Core options.

### NAOMI and Atomiswave

Zips go in `roms/naomi/` and `roms/atomiswave/` as they are, with `naomi.zip` /
`awbios.zip` beside them or in `system/dc/`. Each set is checked against
Flycast's own game list (`cores/flycast_libretro.db`, made by
`tools/make_arcade_db.py` from Flycast's `naomi_roms.cpp`), the same way as the
arcade folder: real names, missing BIOS or parent zips named, vertical games
(Ikaruga...) on the playfield. GD-ROM games also need their `.chd` in a folder
named after the zip, e.g. `roms/naomi/ikaruga/gdl-0010.chd`.

### Trackball (Arcade Control Panel)

The Arcade Control Panel's trackball works in trackball, spinner and paddle
games, for example Missile Command, Centipede, Marble Madness, Tempest and
Arkanoid.
- **How it's read:** the player reads it as a USB mouse
  (`/dev/input/by-id/usb-0838_8918-event-mouse`, as the firmware does). Any
  other USB mouse works too.
- **MAME 2003-Plus:** gets the trackball as its mouse.
- **FBNeo:** switches to the trackball for the games MAME 2003-Plus's list marks
  as trackball, dial or paddle (flag `T` in the arcade databases).
- **Menus:** rolling the trackball up or down scrolls any list, stopping at the
  ends (not while Home > Controls waits for a button).
- **Speed:** Settings > Trackball speed (Normal, Slow, Fast), for games and for
  scrolling.

The log lists every input device at startup (`input:` lines) and the trackball
it found (`trackball:`).

### Neo Geo

Neo Geo games have their own row, with the console photo and SNK covers. Put
the zips in `roms/neogeo/` as they are: FBNeo sets such as `mslug.zip` or
`kof98.zip`, with any clone's parent zip beside it. FBNeo plays them, and each
zip is checked against its ROM list like the arcade games are. Zips that aren't
Neo Geo games are listed with a note to move them to `roms/arcade/`.

The BIOS, `neogeo.zip`, goes with the games or in `system/fbneo/`. If you only
have it in `roms/arcade/`, it's copied to `system/fbneo/` for you. The cabinet
buttons A, B, Y and X are the Neo Geo's A, B, C and D. Rewind inserts a coin.
Neo Geo games in `roms/arcade/` still play there too.

### Arcade

Put arcade zips in `roms/arcade/`, as they are (don't unpack them), with any
parent and BIOS zips they need (`sf2.zip` for `sf2ce.zip`, `neogeo.zip` for Neo
Geo games) in the same folder.

Arcade sets only work with the emulator version they were made for. Retro
Launcher checks each zip against the ROM list of both emulators (from the CRCs
in the zip's index, without unpacking it) and plays it with the one it is
complete for: **FBNeo** when both can (current FBNeo sets), else **MAME
2003-Plus** (MAME 0.78-era sets). A set that is complete for neither is still
listed, dimmed, with the reason: a missing parent or BIOS zip, or ROMs from a
different version. Home on a game lets you pick the emulator yourself.

Picking **Arcade** first shows its genres: All games, Vertical games, then
Shooters, Platformers, Fighters, Sports and the rest (from MAME's `catver.ini`,
made into `cores/arcade-genres.txt` by `tools/make_arcade_db.py`). B in a genre
goes back to the list; the genre used last is remembered. Settings > **Arcade:
hide clones & broken sets** leaves out regional / revision variants whose parent
zip you also have, and sets that are incomplete, unknown or marked not working
(nothing is deleted). **High scores** are kept: MAME 2003-Plus has its own list,
and FBNeo uses `system/fbneo/hiscore.dat` (from FBNeo's `metadata/`), saving each
game's table in `system/fbneo/`.

Games are listed by their real names ("Street Fighter II': Champion Edition"),
with year and maker. **Vertical games** (1942, Galaga, DoDonPachi...) play on
the playfield by default, filling it in portrait; horizontal ones on the
backglass. Home on a game changes either. Rewind is the coin button.

The ROM lists are `cores/fbneo_libretro.db`, `cores/mame2003_plus_libretro.db` and
`cores/flycast_libretro.db` (NAOMI / Atomiswave),
made by `tools/make_arcade_db.py` from the same source the cores are built from.

## Installing

1. Build the app (see [Building](#building)), or take a release.
2. Copy the `retro-launcher` folder to `external/retro-launcher/` on the USB
   stick. It needs `retro-launcher.elf`, `retro-launcher.png` and
   `retro-launcher.xml`.
3. Optionally add `cores/` with the cores from a release or your own build.
4. Put your games in `roms/<system>/`. The app creates the folders on first run,
   each with a `README.txt` saying what goes in it (file types, BIOS names, core
   names, artwork naming). It never overwrites one you have edited.

Everything the app uses lives in its own folder:

```
external/retro-launcher/
  retro-launcher.elf / .png / .xml
  roms/<system>/        your games (plain or .zip)
  saves/<system>/       battery saves and save states
  system/               BIOS files the cores ask for (gba_bios.bin, lynxboot.img...)
  cores/                optional cores that replace or add to the firmware's
  media/<system>/       bezel.png, console.png|jpg, boxart/<ROM name>.png|jpg
  data/                 settings.cfg, core-options.cfg, recent.txt, launcher.log
```

### Network transfer

**Settings > Network transfer** lets you send games to the cabinet without taking
out the USB stick:

1. Open it on the cabinet. It shows an address, such as
   `http://192.168.1.20:8080`, and a 4-digit PIN.
2. Open that address in a browser on any computer or phone on the same network,
   and enter the PIN.
3. Pick a system, then drop your games on the page or choose them.

**How files are handled:**
- Files go straight into `roms/<system>/`. Games already on the stick are skipped.
- Disc systems (PlayStation, Saturn, Dreamcast, PSP) and NAOMI/Atomiswave can take
  a game as a folder, such as a .cue with its .bin tracks. For other systems,
  folders are flattened.
- The page accepts only the system's own file types.

**While it's open:**
- The transfer runs only while the screen is open. Each time it opens, it uses a
  new random PIN.
- Every request needs the PIN, and nothing outside `roms/` can be written.
- Press B when you're done. The menu then picks up the new games.

### Artwork

The repository contains no artwork.

**From the cabinet:** connect it to the internet, then pick **Settings >
Download artwork**. This downloads covers and logos for your games from
[libretro-thumbnails](https://github.com/libretro-thumbnails), and a bezel (the
frame around the picture) for each system you have games for, from
[The Bezel Project](https://github.com/thebezelproject) (personal use; there is
none for PSP):

- **Only what's missing.** Games that already have a cover or logo are skipped,
  and so are files already on the stick.
- **Lists are fetched once a week.** Each system's list of covers comes from
  thumbnails.libretro.com. Sets that server lacks come from GitHub instead
  (arcade logos and a few others). The lists are kept in `media/.art-index/`.
  Running it again within the week downloads only new files.
- **Matching:** files are matched by title, in the same way as prefilled covers.
  They are saved as PNG in `media/<system>/Named_Boxarts/` and `Named_Logos/`.
- **Unmatched games** are listed in `media/art-not-found.txt`.

### Multi-disc games

PlayStation and Dreamcast discs named `… (Disc 1)`, `(Disc 2)`… (also
`(Disc 1 of 3)`, `(CD1)`) with no playlist yet get one when the menu scans:
- discs together in one folder get `<game>.m3u` beside them, or `<folder>.m3u`
  inside a game's own folder;
- one disc per folder (`Game (Disc 1)/`, `Game (Disc 2)/`…) gets `<game>.m3u`
  next to those folders, pointing into each;
- disc folders inside a game folder (`Game/Game (Disc 1)/`…) get `Game/Game.m3u`. The menu then lists one game: its disc files are hidden, it
has one memory card, and pause menu > Change disc swaps discs. The newest
per-disc `.srm` save is copied to the game's save name if it has none yet.
Zipped discs can't be played; unzip them or convert them to `.chd`. The Saturn
emulator (YabaSanshiro) reads no playlists and can't swap discs, so a Saturn
multi-disc game lists each disc on its own.

The download runs in the background while you browse. Starting a game stops it;
pick it again later to continue. It uses the firmware's `curl`.

**From a computer:** these scripts download the artwork into the app folder on
your USB stick. On macOS they use `sips` to convert the images.

```sh
tools/fetch_media.sh  /Volumes/USB/external/retro-launcher           # bezels + console photos
tools/fetch_boxart.py /Volumes/USB/external/retro-launcher [systems]  # box art for your ROMs
```

Or download every cover for every system up front (about 34,000 covers, ~5 GB),
so games you add later already have art:

```sh
tools/prefill_boxart.py /Volumes/USB/external/retro-launcher [systems]
```

Prefilled covers go to `media/<system>/Named_Boxarts/` under their No-Intro
names. The launcher matches a ROM to them by title, ignoring tags, case and
punctuation and preferring the ROM's region, so GoodTools-style names like
`Streets of Rage 2 (U) [!].bin` find their cover too.

`fetch_boxart.py` matches your ROM file names (GoodNES style, plain titles or
squashed names) to [libretro-thumbnails](https://github.com/libretro-thumbnails)
and saves each cover under the ROM's own name. Games it cannot match are listed
in `media/boxart-missing.txt`; add those by hand with the same file name as the
ROM.

## Controls

| Where | Control | Action |
|---|---|---|
| Lists | Up / Down (D-pad or joystick) | Move; hold to scroll, faster the longer you hold |
| Lists | A or Start | Open / play |
| Lists | Flippers | Previous / next letter in a games list, a page elsewhere |
| Games list | Second flippers | Previous / next system |
| Game lists | Rewind (or Y) | Add to / remove from Favorites |
| Game lists | Home | Game options: Play, Favorite, Screen (default / backglass / playfield), Emulator (arcade), Controls, Search |
| Controls | Up / Down, Left / Right, A | Layout for this game or the whole system, a preset, or A on a button then press the cabinet button for it |
| Consoles | Home or X | Search |
| Lists | B | Back; on the consoles list, asks before exiting |
| Settings | Left / Right | Change the highlighted setting |
| Search | Left / Right / Up / Down | Move on the keyboard |
| Search | A / left flipper / right flipper | Type / delete / jump to results |
| In a game | Rewind | Select (Coin in arcade games) |
| In a game | Hold Start (1 s) or Home | Pause menu |
| Pause menu | A / B or Start | Select / resume |

Cabinet A/B/X/Y map to libretro B/A/Y/X (SNES layout) by default. **Home >
Controls** on a game changes it, for that game or its whole system: a preset
(Default, Swap A and B, Flippers as A and B) or button by button (A on a button,
then press the cabinet button for it; a button already in use swaps jobs). Saved
as `controls.<system>` / `controls.<system>/<file>` in `data/settings.cfg`. Menus,
the pause menu and the directions always use the default layout. The playfield
card shows the layout in force while you play.

## Settings

The **Settings** row (under Favorites on the consoles list) changes these; they are
stored in `data/settings.cfg` (key = value):

| Setting | Key | Values | Default |
|---|---|---|---|
| Picture sides | `sides` | `bezel`, `glow`, `black` | `bezel` |
| Scaling | `scaling` | `smooth`, `sharp`, `integer` (pixel-perfect) | `smooth` |
| CRT scanlines | `scanlines` | `off`, `light`, `strong` | `off` |
| Screen artwork | `panels` | `on`, `off` | `on` |
| Default game screen | `screen.default` | `backglass`, `playfield` | `backglass` |
| Playfield game rotation | `rotate.playfield` | `90`, `270` | `90` |
| Arcade: hide clones & broken sets | `arcade.hide` | `off`, `on` | `off` |
| Hide consoles with no games | `systems.hideEmpty` | `off` (listed, dimmed), `on` | `off` |
| Trackball speed | `trackball.speed` | `normal`, `slow`, `fast` | `normal` |

Below the settings are five actions:
- **Download artwork** adds covers and logos for games that have none (see Artwork).
  Press A again while it runs to stop it.
- **Network transfer** sends games from a browser (see Network transfer).
- **BIOS check** lists the BIOS files the systems you have games for can use. For
  each file it shows:
  - whether it is **Required** or **Good to have**, and why;
  - whether it is found and is the known good dump (checked against the MD5s in
    libretro's `System.dat`);
  - if it is missing, which folder to put it in.

  It also lists the BIOS and parent zips your arcade sets still need. The same
  report is written to `data/bios-report.txt` to read on a computer. Retro
  Launcher downloads no BIOS files.
- **Updates** checks the latest [GitHub release](https://github.com/Bla1ze/retro-launcher/releases).
  A on a newer version downloads its zip and replaces only the files that changed
  (compared by size and CRC-32). Every file is unpacked and checked first, then
  moved into place, the app itself last, and the menu restarts into the new
  version.
  - A release holds only the app, cores, PPSSPP files, licenses and console
    pictures. Games, saves, settings, downloaded artwork and BIOS files are never
    touched.
  - Your own `retro-launcher.png` / `.xml` are kept.
  - The menu also checks once a day by itself, quietly, and says when a new
    version is out.
- **Empty trash** deletes the games removed with Home > Remove game.

Per-game screens are stored as `game.<system>/<rom file> = backglass|playfield`.

Core options changed in the pause menu are saved to `data/core-options.cfg` in
RetroArch's format (`option_key = "value"`), so options from a RetroArch setup can
be copied in.

## Building

Retro Launcher builds with the toolchain from the **AtGames External
Applications SDK** ([atgames.net/features/external-apps](https://www.atgames.net/features/external-apps)):
a Docker image with an aarch64 cross-compiler, SDL2 2.28 and a glibc 2.26
sysroot. Place this repository in the SDK's `apps/` folder as
`apps/retro-launcher` and build it like any SDK app:

```sh
docker run --rm --platform linux/amd64 -v "$SDK:/workspace" \
  atgames-external-sdk:glibc-2.26-sdl2-v1 \
  /workspace/scripts/build-app.sh retro-launcher
```

The output is `dist/external/retro-launcher/`. The build ends with the SDK's
compatibility check (glibc ≤ 2.26). The SDK's toolchain file sets no
optimization level, so this project forces a Release (`-O3`) build itself.

### The website and guides

The website is `docs/` (GitHub Pages, from `main`). The guides under `docs/wiki/`
are generated:
1. Edit a guide in `site/wiki/<page>.html`, or the system data in
   `tools/build_wiki.py`.
2. Run `tools/build_wiki.py`.
3. Commit both.

### Making a release

```sh
tools/make_release.sh /path/to/staging/external/retro-launcher   # -> release/retro-launcher-v<version>.zip
```

The zip's only top-level item is `external/` (with `INSTALL.txt` inside
`external/retro-launcher/`), so unzipping it gives a folder ready to copy to the
root of a USB stick. The script copies only an allow-list from the staging
folder:
- the app;
- the cores with `CORES.txt`;
- PPSSPP's support files and FBNeo's `hiscore.dat`;
- `licenses/`;
- the console pictures with their credits.

It then refuses to finish if anything personal or downloaded got in: games,
saves, settings, logs, BIOS or zips, box art, logos or bezels.

To publish it, attach the zip to a GitHub release tagged `v<version>`, for example:

```sh
gh release create v0.23.0 release/retro-launcher-v0.23.0.zip
```

Settings > Updates on a cabinet looks for the newest release with a
`retro-launcher-v*.zip` file attached.

### Building the cores

Prebuilt libretro cores from the libretro buildbot need glibc 2.29+ and do not
load on the cabinet, so the cores are built with the SDK toolchain:

```sh
mkdir -p "$SDK/cores-src" && cd "$SDK/cores-src"
for r in libretro/snes9x libretro/libretro-fceumm drhelius/Gearcoleco \
         libretro/gambatte-libretro libretro/gpsp \
         libretro/beetle-pce-fast-libretro libretro/libretro-handy \
         libretro/FBNeo libretro/mame2003-plus-libretro libretro/pcsx_rearmed; do
  git clone --depth 1 "https://github.com/$r.git"
done
git -C gpsp apply /path/to/retro-launcher/tools/patches/gpsp-arm64-old-gas.patch
git -C pcsx_rearmed apply /path/to/retro-launcher/tools/patches/pcsx_rearmed-arm64-old-gas.patch
cp /path/to/retro-launcher/tools/build-cores.sh .
docker run --rm --platform linux/amd64 -v "$SDK:/workspace" \
  atgames-external-sdk:glibc-2.26-sdl2-v1 bash /workspace/cores-src/build-cores.sh
```

Flycast (Dreamcast, NAOMI, Atomiswave), PPSSPP (PSP), Mupen64Plus-Next (N64) and
YabaSanshiro (Saturn; `tools/build-gpu-cores.sh` builds those two) need a newer compiler than the SDK's;
`tools/build-flycast.sh` describes the toolchain, and `tools/build-ppsspp.sh`
adds what PPSSPP needs (Python for its build, and the cabinet's own
`libGLESv2`/`libEGL`/`libmali` to link against, from the firmware).

The cores land in `cores-src/out/`. Copy them to the app's `cores/` folder, and
make the arcade ROM lists there from the same checkouts:

```sh
tools/make_arcade_db.py "$SDK/cores-src" /Volumes/USB/external/retro-launcher/cores
```

## How it works

- **One binary, two modes.** The menu and the game player are the same ELF. A
  launch `exec`s it in play mode, and quitting a game `exec`s back to the menu,
  so the process ID the firmware's launcher waits on stays alive.
- **Screens.** The cabinet's SDL picks its output from the `ForceConnectID`
  environment variable, set per screen before `SDL_Init`. Which connector is
  which screen depends on the model (`ATGAMES_DEVICE_TYPE_ID`). The other screens
  are driven directly over KMS with dumb buffers on SDL's own DRM file
  descriptor.
- **Cores.** Cores are `dlopen`ed: from the app's `cores/` folder (copied to
  `/tmp` first, since the USB stick is mounted no-exec), else from the firmware's
  core folders. Zipped ROMs are unpacked with the cabinet's own `libz.so.1`.

## Credits and sources

**Code included in this repository**

| Component | Source | License |
|---|---|---|
| Controls helper (`src/controls/`), font renderer base (`src/AppFont.cpp`), screen layout table (`src/DisplayProfile.h`) | AtGames External Applications SDK, [atgames.net/features/external-apps](https://www.atgames.net/features/external-apps), modified | No license stated; included with attribution to AtGames |
| `libretro_min.h` (subset of `libretro.h`) | [libretro-common](https://github.com/libretro/libretro-common) | MIT |
| `stb_truetype.h`, `stb_image.h` | [nothings/stb](https://github.com/nothings/stb), Sean Barrett | Public domain / MIT |
| `drm_mode.h` | Linux kernel DRM UAPI | MIT |
| Noto Sans | [Google Fonts](https://fonts.google.com/noto/specimen/Noto+Sans) | SIL OFL 1.1 (`assets/fonts/OFL.txt`) |
| Bebas Neue | [Dharma Type / Google Fonts](https://fonts.google.com/specimen/Bebas+Neue) | SIL OFL 1.1 (`assets/fonts/BebasNeue-OFL.txt`) |
| Root certificates ISRG Root X1, USERTrust ECC and RSA (in `src/ArtDownload.cpp`) | [Internet Security Research Group](https://letsencrypt.org/certificates/) / [Sectigo](https://www.sectigo.com/) | Public trust anchors, added to the firmware's CA bundle for the artwork servers |

Both fonts are built into the app. Their license texts, and this project's GPL,
are copied into the app folder's `licenses/` with every build.

**Data the tools build from** (generated into the app folder, not stored here)

| Data | Source | Used for |
|---|---|---|
| FBNeo ROM list (`dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat`) | [libretro/FBNeo](https://github.com/libretro/FBNeo) | `cores/fbneo_libretro.db`: recognising arcade sets, names, orientation |
| MAME 2003-Plus ROM list (`metadata/mame2003-plus.xml`) | [libretro/mame2003-plus-libretro](https://github.com/libretro/mame2003-plus-libretro) | `cores/mame2003_plus_libretro.db` |
| Flycast NAOMI / Atomiswave list (`core/hw/naomi/naomi_roms.cpp`) | [flyinghead/flycast](https://github.com/flyinghead/flycast) | `cores/flycast_libretro.db` |
| `catver.ini` (via MAME 2003-Plus's `metadata/`) | [Progetto-SNAPS](https://www.progettosnaps.net/catver/), AntoPISA | `cores/arcade-genres.txt`: arcade genres |
| `hiscore.dat` (via FBNeo's `metadata/`) | The MAME hiscore.dat project (Leezer and contributors) | FBNeo high scores (`system/fbneo/`) |

**Build tools** (used to build the cores, not distributed)

- [Bootlin toolchains](https://toolchains.bootlin.com/) (`aarch64--glibc--bleeding-edge-2017.11-1`: GCC 7.2, glibc 2.26) for Flycast, PPSSPP, Mupen64Plus-Next and YabaSanshiro.
- [Khronos OpenGL ES / EGL headers](https://github.com/KhronosGroup/OpenGL-Registry) (Apache 2.0 / MIT).
- [python-build-standalone](https://github.com/astral-sh/python-build-standalone) (Python for PPSSPP's build), [CMake](https://cmake.org/).
- `tools/compat/filesystem` and `tools/compat/aarch64_atomics.c` are this project's (GPL-3.0), standing in for parts of newer GCC versions.

**Emulator cores** (not in this repository; built from these sources)

| Core | Source | License |
|---|---|---|
| Snes9x | [libretro/snes9x](https://github.com/libretro/snes9x) | Snes9x license (non-commercial) |
| FCEUmm | [libretro/libretro-fceumm](https://github.com/libretro/libretro-fceumm) | GPL-2.0 |
| Gearcoleco | [drhelius/Gearcoleco](https://github.com/drhelius/Gearcoleco) | GPL-3.0 |
| Gambatte | [libretro/gambatte-libretro](https://github.com/libretro/gambatte-libretro) | GPL-2.0 |
| gpSP | [libretro/gpsp](https://github.com/libretro/gpsp) (with `tools/patches/gpsp-arm64-old-gas.patch`) | GPL-2.0 |
| Beetle PCE Fast | [libretro/beetle-pce-fast-libretro](https://github.com/libretro/beetle-pce-fast-libretro) | GPL-2.0 |
| Handy | [libretro/libretro-handy](https://github.com/libretro/libretro-handy) | zlib |
| PCSX ReARMed | [libretro/pcsx_rearmed](https://github.com/libretro/pcsx_rearmed) (with `tools/patches/pcsx_rearmed-arm64-old-gas.patch`) | GPL-2.0 |
| Mupen64Plus-Next | [libretro/mupen64plus-libretro-nx](https://github.com/libretro/mupen64plus-libretro-nx) (`tools/build-gpu-cores.sh`) | GPL-2.0 |
| YabaSanshiro | [libretro/yabause, yabasanshiro branch](https://github.com/libretro/yabause/tree/yabasanshiro) (`tools/build-gpu-cores.sh`) | GPL-2.0 |
| PPSSPP | [hrydgard/ppsspp](https://github.com/hrydgard/ppsspp) (with `tools/patches/ppsspp-gcc7.patch`, built by `tools/build-ppsspp.sh`; its `assets/` go to `system/PPSSPP/`) | GPL-2.0 |
| Flycast | [flyinghead/flycast](https://github.com/flyinghead/flycast) (with `tools/patches/flycast-gcc7.patch`, built by `tools/build-flycast.sh`) | GPL-3.0 |
| FBNeo | [libretro/FBNeo](https://github.com/libretro/FBNeo) | FBNeo license (non-commercial) |
| MAME 2003-Plus | [libretro/mame2003-plus-libretro](https://github.com/libretro/mame2003-plus-libretro) | MAME license (non-commercial) |

The firmware's own cores (Genesis Plus GX, QuickNES, SNES Faust, Stella, and
its ColecoVision core `libcv`, listed as "Marat CV" in the firmware) are loaded
from the cabinet at run time and are not distributed here.

**Artwork** (downloaded by Settings > Download artwork or the tools, not in this repository)

- Box art and game logos (marquees): [libretro-thumbnails](https://github.com/libretro-thumbnails)
  (`Named_Boxarts`, `Named_Logos`; arcade logos from its MAME set).
- Bezels: [The Bezel Project](https://github.com/thebezelproject), fan-made, for personal use.
  Each cabinet downloads its own (Settings > Download artwork); releases don't include them.
- Console photos: Evan Amos, [Wikimedia Commons](https://commons.wikimedia.org/wiki/User:Evan-Amos).
  Public domain, except the SNES, Atari Lynx, Dreamcast and Neo Geo photos, which are
  [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/). The app changes them on
  screen (white background removed, cropped and resized). Each photo's file name, author
  and license are in `media/<system>/console.credit` and `media/CREDITS.txt`.
- Arcade cabinet, NAOMI board and Atomiswave board: drawn for Retro Launcher (`tools/draw_consoles.py`,
  `assets/consoles/`), under this repository's license.

Thanks to the libretro and RetroArch developers, the authors of every core
listed above, the libretro-thumbnails contributors, The Bezel Project, Evan Amos,
Progetto-SNAPS (catver.ini), the hiscore.dat maintainers, Bootlin, and AtGames
for opening the cabinets to External Applications.

## License

Retro Launcher's own code is released under the
[GNU General Public License v3.0](LICENSE). Third-party components keep their
own licenses, listed above.
