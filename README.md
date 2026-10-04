# Retro Launcher

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
  every system with an on-screen keyboard, Recently played, Favourites, A–Z
  letter jumps on the flippers, and an in-app Settings screen.
- **Choose the screen per game**: backglass or playfield (Home on a game),
  with a default in Settings. The screens a game isn't using show artwork.
- **Backglass and DMD while browsing**: the highlighted game's box art over a
  blurred copy of itself on the backglass, and its title on the DMD. Games
  without art get a generated cover.
- **Games on the backglass** (or the playfield), with the picture shape correct
  per system. Bezels (The Bezel Project format) or an ambient glow taken from the
  game's own colours fill the sides. Smooth, sharp or pixel-perfect scaling, and
  optional CRT scanlines. Blank edge columns (Master System, NES) are cropped so the
  picture is centred.
- **While playing**: a "Now playing" card on the playfield with the cover and the
  game's controls, and a photo of the console on the DMD.
- **Pause menu**: hold Start or press Home for Resume, Save state, Load state,
  Reset, Core options and Quit. Core options lists every setting the running
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
| ColecoVision | `roms/colecovision` | Gearcoleco (else firmware) | `cores/` |
| Game Boy | `roms/gb` | Gambatte | `cores/` |
| Game Boy Color | `roms/gbc` | Gambatte | `cores/` |
| Game Boy Advance | `roms/gba` | gpSP | `cores/` |
| PC Engine / TurboGrafx-16 | `roms/pce` | Beetle PCE Fast | `cores/` |
| Atari Lynx | `roms/lynx` | Handy | `cores/` |

Genesis is verified on a Legends Pinball 4KP; the others are marked "untested"
in the menu until confirmed. ROMs can be plain files or `.zip`.

## Installing

1. Build the app (see [Building](#building)), or take a release.
2. Copy the `retro-launcher` folder to `external/retro-launcher/` on the USB
   stick. It needs `retro-launcher.elf`, `retro-launcher.png` and
   `retro-launcher.xml`.
3. Optionally add `cores/` with the cores from a release or your own build.
4. Put your games in `roms/<system>/`. The app creates the folders on first run.

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

### Artwork

The repository contains no artwork. Two scripts download it into the app folder
on your USB stick (macOS: they use `sips` to convert images):

```sh
tools/fetch_media.sh  /Volumes/USB/external/retro-launcher           # bezels + console photos
tools/fetch_boxart.py /Volumes/USB/external/retro-launcher [systems]  # box art for your ROMs
```

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
| Game lists | Rewind (or Y) | Add to / remove from Favourites |
| Game lists | Home | Game options: Play, Favourite, Screen (default / backglass / playfield), Search |
| Consoles | Home or X | Search |
| Lists | B | Back; on the consoles list, asks before exiting |
| Settings | Left / Right | Change the highlighted setting |
| Search | Left / Right / Up / Down | Move on the keyboard |
| Search | A / left flipper / right flipper | Type / delete / jump to results |
| In a game | Hold Start (1 s) or Home | Pause menu |
| Pause menu | A / B or Start | Select / resume |

Cabinet A/B/X/Y map to libretro B/A/Y/X (SNES layout). The playfield card shows
each system's mapping while you play.

## Settings

The **Settings** row at the bottom of the consoles list changes these; they are
stored in `data/settings.cfg` (key = value):

| Setting | Key | Values | Default |
|---|---|---|---|
| Picture sides | `sides` | `bezel`, `glow`, `black` | `bezel` |
| Scaling | `scaling` | `smooth`, `sharp`, `integer` (pixel-perfect) | `smooth` |
| CRT scanlines | `scanlines` | `off`, `light`, `strong` | `off` |
| Screen artwork | `panels` | `on`, `off` | `on` |
| Default game screen | `screen.default` | `backglass`, `playfield` | `backglass` |
| Playfield game rotation | `rotate.playfield` | `90`, `270` | `90` |

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
optimisation level, so this project forces a Release (`-O3`) build itself.

### Building the cores

Prebuilt libretro cores from the libretro buildbot need glibc 2.29+ and do not
load on the cabinet, so the cores are built with the SDK toolchain:

```sh
mkdir -p "$SDK/cores-src" && cd "$SDK/cores-src"
for r in libretro/snes9x libretro/libretro-fceumm drhelius/Gearcoleco \
         libretro/gambatte-libretro libretro/gpsp \
         libretro/beetle-pce-fast-libretro libretro/libretro-handy; do
  git clone --depth 1 "https://github.com/$r.git"
done
git -C gpsp apply /path/to/retro-launcher/tools/patches/gpsp-arm64-old-gas.patch
cp /path/to/retro-launcher/tools/build-cores.sh .
docker run --rm --platform linux/amd64 -v "$SDK:/workspace" \
  atgames-external-sdk:glibc-2.26-sdl2-v1 bash /workspace/cores-src/build-cores.sh
```

The cores land in `cores-src/out/`. Copy them to the app's `cores/` folder.

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

| Component | Source | Licence |
|---|---|---|
| Controls helper (`src/controls/`), font renderer base (`src/AppFont.cpp`), screen layout table (`src/DisplayProfile.h`) | AtGames External Applications SDK, [atgames.net/features/external-apps](https://www.atgames.net/features/external-apps), modified | No licence stated; included with attribution to AtGames |
| `libretro_min.h` (subset of `libretro.h`) | [libretro-common](https://github.com/libretro/libretro-common) | MIT |
| `stb_truetype.h`, `stb_image.h` | [nothings/stb](https://github.com/nothings/stb), Sean Barrett | Public domain / MIT |
| `drm_mode.h` | Linux kernel DRM UAPI | MIT |
| Noto Sans | [Google Fonts](https://fonts.google.com/noto/specimen/Noto+Sans) | SIL OFL 1.1 (`assets/fonts/OFL.txt`) |
| Bebas Neue | [Dharma Type / Google Fonts](https://fonts.google.com/specimen/Bebas+Neue) | SIL OFL 1.1 (`assets/fonts/BebasNeue-OFL.txt`) |

**Emulator cores** (not in this repository; built from these sources)

| Core | Source | Licence |
|---|---|---|
| Snes9x | [libretro/snes9x](https://github.com/libretro/snes9x) | Snes9x licence (non-commercial) |
| FCEUmm | [libretro/libretro-fceumm](https://github.com/libretro/libretro-fceumm) | GPL-2.0 |
| Gearcoleco | [drhelius/Gearcoleco](https://github.com/drhelius/Gearcoleco) | GPL-3.0 |
| Gambatte | [libretro/gambatte-libretro](https://github.com/libretro/gambatte-libretro) | GPL-2.0 |
| gpSP | [libretro/gpsp](https://github.com/libretro/gpsp) (with `tools/patches/gpsp-arm64-old-gas.patch`) | GPL-2.0 |
| Beetle PCE Fast | [libretro/beetle-pce-fast-libretro](https://github.com/libretro/beetle-pce-fast-libretro) | GPL-2.0 |
| Handy | [libretro/libretro-handy](https://github.com/libretro/libretro-handy) | zlib |

The firmware's own cores (Genesis Plus GX, QuickNES, SNES Faust, Stella and
others) are loaded from the cabinet at run time and are not distributed here.

**Artwork** (downloaded by the tools, not in this repository)

- Box art: [libretro-thumbnails](https://github.com/libretro-thumbnails) (`Named_Boxarts`).
- Bezels: [The Bezel Project](https://github.com/thebezelproject), fan-made, for personal use.
- Console photos: Evan Amos, [Wikimedia Commons](https://commons.wikimedia.org/wiki/User:Evan-Amos).
  Public domain, except the SNES and Atari Lynx photos (CC BY-SA 3.0).

Thanks to the libretro and RetroArch developers, the authors of every core
listed above, The Bezel Project, and AtGames for opening the cabinets to
External Applications.

## Licence

Retro Launcher's own code is released under the
[GNU General Public License v3.0](LICENSE). Third-party components keep their
own licences, listed above.
