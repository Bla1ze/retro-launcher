#!/usr/bin/env python3
"""Download every libretro-thumbnails box art cover for every system, whether or
not you have the game yet, into <app>/media/<system>/Named_Boxarts/<name>.jpg.

The launcher finds a ROM's cover there by its No-Intro name, and otherwise by
title (tags, case and punctuation ignored, region from the ROM's tags), so games
you add later get art without running anything.

Usage:  prefill_boxart.py <app folder> [system ...] [--logos]
        --logos fetches game logos (for the DMD) into media/<system>/Named_Logos/ instead.
Needs macOS `sips` and network access. Safe to stop and rerun: covers already
downloaded are skipped. About 46,000 covers, ~20 GB to download, ~7 GB stored.
"""
import concurrent.futures as cf
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

REPOS = {
    # Arcade covers are named by the game's full title, which the launcher
    # matches against each set's name from its arcade database.
    "arcade": ["FBNeo_-_Arcade_Games", "MAME"],
    "genesis": ["Sega_-_Mega_Drive_-_Genesis"],
    "mastersystem": ["Sega_-_Master_System_-_Mark_III"],
    "gamegear": ["Sega_-_Game_Gear"],
    "nes": ["Nintendo_-_Nintendo_Entertainment_System"],
    "snes": ["Nintendo_-_Super_Nintendo_Entertainment_System"],
    "atari2600": ["Atari_-_2600"],
    "colecovision": ["Coleco_-_ColecoVision"],
    "gb": ["Nintendo_-_Game_Boy"],
    "gbc": ["Nintendo_-_Game_Boy_Color"],
    "gba": ["Nintendo_-_Game_Boy_Advance"],
    "pce": ["NEC_-_PC_Engine_-_TurboGrafx_16", "NEC_-_PC_Engine_SuperGrafx"],
    "lynx": ["Atari_-_Lynx"],
    "psx": ["Sony_-_PlayStation"],
    "dreamcast": ["Sega_-_Dreamcast"],
    "psp": ["Sony_-_PlayStation_Portable"],
    "n64": ["Nintendo_-_Nintendo_64"],
    "saturn": ["Sega_-_Saturn"],
    "naomi": ["Sega_-_Naomi", "Sega_-_Naomi_2"],
    "atomiswave": ["Atomiswave"],
}
UA = {"User-Agent": "retro-launcher-prefill"}


def get(url, timeout=120):
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=timeout) as r:
                return r.read()
        except Exception:  # noqa: BLE001
            if attempt == 3:
                raise
            time.sleep(2 + attempt * 3)


# --logos: game logos (Named_Logos) for the DMD instead of covers. Arcade, NAOMI
# and Atomiswave share the MAME set (the launcher looks in media/arcade/ for them).
LOGO_REPOS = dict(REPOS, arcade=["MAME"], naomi=[], atomiswave=[])


def covers(app, repo, folder="Named_Boxarts"):
    """[(name, branch)] from the repo's Named_Boxarts (or Named_Logos) tree,
    cached a week (the GitHub API allows 60 unauthenticated calls an hour)."""
    suffix = "" if folder == "Named_Boxarts" else "-" + folder
    cache = os.path.join(app, "media", ".boxart-trees", repo + suffix + ".json")
    if not (os.path.exists(cache) and time.time() - os.path.getmtime(cache) < 7 * 86400):
        root, branch = None, None
        for branch in ("master", "main"):  # most sets use master, a few (NAOMI) main
            try:
                root = json.loads(get(f"https://api.github.com/repos/libretro-thumbnails/{repo}/git/trees/{branch}"))
                break
            except urllib.error.HTTPError:
                continue
        sha = next((t["sha"] for t in root["tree"] if t["path"] == folder), None)
        if sha is None:  # this set has no logos
            return []
        tree = json.loads(get(f"https://api.github.com/repos/libretro-thumbnails/{repo}/git/trees/{sha}"))
        tree["branch"] = branch
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, "w") as f:
            json.dump(tree, f)
    with open(cache) as f:
        tree = json.load(f)
    branch = tree.get("branch", "master")
    return [(t["path"], branch) for t in tree["tree"] if t["path"].lower().endswith(".png")]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args or not os.path.isdir(args[0]):
        print(__doc__)
        return 2
    app, only = args[0], args[1:]
    logos = "--logos" in sys.argv
    folder = "Named_Logos" if logos else "Named_Boxarts"
    jobs = []  # (url, dest)
    for sys_id, repos in (LOGO_REPOS if logos else REPOS).items():
        if only and sys_id not in only:
            continue
        if not repos:
            continue
        out = os.path.join(app, "media", sys_id, folder)
        os.makedirs(out, exist_ok=True)
        have = set(os.listdir(out))
        todo = 0
        for repo in repos:
            for name, branch in covers(app, repo, folder):
                dest = name[:-4] + (".png" if logos else ".jpg")  # logos keep their transparency
                if dest in have:
                    continue
                have.add(dest)
                url = f"https://raw.githubusercontent.com/libretro-thumbnails/{repo}/{branch}/{folder}/{urllib.parse.quote(name)}"
                jobs.append((url, os.path.join(out, dest)))
                todo += 1
        print(f"{sys_id}: {len(have)} {'logos' if logos else 'covers'}, {todo} to download", flush=True)

    done, failed, lock, t0 = [0], [], threading.Lock(), time.time()

    def fetch(job):
        url, dest = job
        tmp = dest + ".part.png"
        try:
            for _ in range(4):
                data = get(url)
                # A duplicate cover is a git symlink: the raw file is the target's name.
                if data[:4] == b"\x89PNG" or len(data) > 1024:
                    break
                url = url.rsplit("/", 1)[0] + "/" + urllib.parse.quote(data.decode("utf-8").strip())
            with open(tmp, "wb") as f:
                f.write(data)
            if dest.endswith(".png"):  # a logo: PNG (alpha), at most 640 px
                subprocess.run(["sips", "-s", "format", "png", "-Z", "640", tmp, "--out", dest], check=True, capture_output=True)
            else:
                subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "85", "-Z", "720", tmp, "--out", dest],
                               check=True, capture_output=True)
        except Exception as e:  # noqa: BLE001
            with lock:
                failed.append(f"{os.path.basename(dest)}: {e}")
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
            with lock:
                done[0] += 1
                if done[0] % 500 == 0 or done[0] == len(jobs):
                    rate = done[0] / max(1.0, time.time() - t0)
                    print(f"  {done[0]}/{len(jobs)}  ({rate:.1f}/s, ~{(len(jobs) - done[0]) / max(rate, 0.01) / 60:.0f} min left)",
                          flush=True)

    with cf.ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(fetch, jobs))
    print(f"done: {len(jobs) - len(failed)} downloaded, {len(failed)} failed")
    for f in failed[:30]:
        print("  ", f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
