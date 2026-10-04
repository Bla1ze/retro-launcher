#!/usr/bin/env python3
"""Match ROMs in <app>/roms/<system>/ to libretro-thumbnails box art and save each
as <app>/media/<system>/boxart/<rom stem>.jpg (the name the launcher looks for).

Usage:  fetch_boxart.py <app folder> [system ...] [--force]
        e.g. fetch_boxart.py /Volumes/USB/external/retro-launcher nes snes
Needs macOS `sips` (to convert covers to JPEG) and network access.

Matching is on the title with tags removed, reduced to lowercase letters+digits,
which bridges GoodNES "Title (U) [!]", plain "Title", and squashed "titlesubtitle"
names. Region tags on the ROM pick between regional covers. Leftovers get a
strict fuzzy pass (same numbers required, high similarity)."""
import concurrent.futures as cf
import difflib
import json
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
import zipfile

APP = ""  # set from the command line
REPOS = {
    "nes": "Nintendo_-_Nintendo_Entertainment_System",
    "snes": "Nintendo_-_Super_Nintendo_Entertainment_System",
    "genesis": "Sega_-_Mega_Drive_-_Genesis",
    "mastersystem": "Sega_-_Master_System_-_Mark_III",
    "gamegear": "Sega_-_Game_Gear",
    "atari2600": "Atari_-_2600",
    "colecovision": "Coleco_-_ColecoVision",
}
ROM_EXT = {".nes", ".sfc", ".smc", ".md", ".gen", ".smd", ".bin", ".sms", ".gg", ".a26", ".col", ".rom", ".zip"}
GOOD_REGION = {"U": "USA", "E": "Europe", "J": "Japan", "W": "World", "UE": "USA", "JU": "USA", "EU": "Europe"}


def get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "retro-launcher-art"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def boxart_names(repo):
    root = get_json(f"https://api.github.com/repos/libretro-thumbnails/{repo}/git/trees/master")
    sha = next(t["sha"] for t in root["tree"] if t["path"] == "Named_Boxarts")
    tree = get_json(f"https://api.github.com/repos/libretro-thumbnails/{repo}/git/trees/{sha}")
    return [t["path"] for t in tree["tree"] if t["path"].lower().endswith(".png")]


def split_tags(stem):
    m = re.search(r"[\(\[]", stem)
    return (stem[: m.start()].strip(), stem[m.start():]) if m else (stem.strip(), "")


def key(title):
    t = title.strip()
    m = re.match(r"^(.*), (The|A|An)$", t)  # "Legend of Zelda, The" -> "The Legend of Zelda"
    if m:
        t = f"{m.group(2)} {m.group(1)}"
    # Roman numerals as digits, so "Street Fighter II" meets "streetfighter2".
    romans = {"ii": "2", "iii": "3", "iv": "4", "v": "5", "vi": "6", "vii": "7", "viii": "8", "ix": "9", "x": "10"}
    t = " ".join(romans.get(w, w) for w in re.split(r"(\s+)", t.lower()) if w.strip() or w == "")
    t = t.replace("&", "and")
    return re.sub(r"[^a-z0-9]", "", t)


def key_variants(title):
    k = key(title)
    out = {k}
    if k.startswith("the"):
        out.add(k[3:])
    return out


def rom_region(tags):
    for code in re.findall(r"\(([A-Za-z]{1,3})\)", tags):
        if code.upper() in GOOD_REGION:
            return GOOD_REGION[code.upper()]
    for word in ("USA", "Europe", "Japan", "World"):
        if word in tags:
            return word
    return "USA"


def pick(candidates, region):
    order = [region, "USA", "World", "Europe", "Japan"]
    def score(name):
        tags = split_tags(name[:-4])[1]
        for i, r in enumerate(order):
            if r in tags:
                # prefer plain releases over (Beta), (Proto), (Rev x) etc.
                return i * 10 + (5 if re.search(r"Beta|Proto|Sample|Demo|Pirate|Unl", tags) else 0) + tags.count("(")
        return 100 + tags.count("(")
    return min(candidates, key=score)


def numbers(k):
    return re.findall(r"\d+", k)


FORCE = "--force" in sys.argv
ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
ONLY = ARGS[1:]


def main():
    global APP
    if not ARGS or not os.path.isdir(os.path.join(ARGS[0], "roms")):
        print(__doc__)
        return 2
    APP = ARGS[0]
    plan = []  # (url, dest)
    report = {}
    for sys_id, repo in REPOS.items():
        if ONLY and sys_id not in ONLY:
            continue
        rom_dir = os.path.join(APP, "roms", sys_id)
        roms = [f for f in sorted(os.listdir(rom_dir)) if not f.startswith(".") and os.path.splitext(f)[1].lower() in ROM_EXT] if os.path.isdir(rom_dir) else []
        if not roms:
            continue
        names = boxart_names(repo)
        index = {}
        for n in names:
            title, _ = split_tags(n[:-4])
            for k in key_variants(title):
                index.setdefault(k, []).append(n)
        keys = list(index)
        out_dir = os.path.join(APP, "media", sys_id, "boxart")
        os.makedirs(out_dir, exist_ok=True)
        matched, fuzzy, missed, skipped = 0, 0, [], 0
        for rom in roms:
            stem = os.path.splitext(rom)[0]
            if not FORCE and any(os.path.exists(os.path.join(out_dir, stem + e)) for e in (".png", ".jpg")):
                skipped += 1
                continue
            title, tags = split_tags(stem)
            # A zip often carries the full name (with region) inside it.
            if rom.lower().endswith(".zip"):
                try:
                    with zipfile.ZipFile(os.path.join(rom_dir, rom)) as z:
                        inner = [n for n in z.namelist() if not n.endswith("/")]
                    if inner:
                        itags = split_tags(os.path.splitext(os.path.basename(inner[0]))[0])[1]
                        if itags and not tags:
                            tags = itags
                except zipfile.BadZipFile:
                    pass
            if re.search(r"\[(b|h|t|o|p|f)\d*", tags):  # bad dumps / hacks / trainers / overdumps
                pass  # still try: art is for the game, not the dump
            cands = None
            for k in key_variants(title):
                if k in index:
                    cands = index[k]
                    break
            if not cands:
                k = key(title)
                close = [c for c in difflib.get_close_matches(k, keys, n=3, cutoff=0.9) if numbers(c) == numbers(k)]
                if close:
                    cands = index[close[0]]
                    fuzzy += 1
            if not cands:
                missed.append(rom)
                continue
            name = pick(cands, rom_region(tags))
            url = f"https://raw.githubusercontent.com/libretro-thumbnails/{repo}/master/Named_Boxarts/{urllib.parse.quote(name)}"
            plan.append((url, os.path.join(out_dir, stem + ".jpg"), name))
            matched += 1
        report[sys_id] = (len(roms), matched, fuzzy, skipped, missed)
        print(f"{sys_id}: {len(roms)} roms, {matched} matched ({fuzzy} fuzzy), {skipped} already had art, {len(missed)} without art", flush=True)

    def fetch(item):
        url, dest, name = item
        tmp = dest + ".png"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "retro-launcher-art"})
            with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as f:
                f.write(r.read())
            # 512px-wide covers as JPEG (opaque art; ~1/6 the size of the PNG).
            subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "85", "-Z", "720", tmp, "--out", dest],
                           check=True, capture_output=True)
            os.remove(tmp)
            return None
        except Exception as e:  # noqa: BLE001
            if os.path.exists(tmp):
                os.remove(tmp)
            return f"{name}: {e}"

    print(f"downloading {len(plan)} covers...", flush=True)
    errors = [e for e in cf.ThreadPoolExecutor(max_workers=12).map(fetch, plan) if e]
    print(f"done, {len(errors)} failed")
    for e in errors[:20]:
        print("  ", e)
    with open(os.path.join(APP, "media", "boxart-missing-" + "-".join(ONLY or ["all"]) + ".txt"), "w") as f:
        for sys_id, (_, _, _, _, missed) in report.items():
            for m in missed:
                f.write(f"{sys_id}\t{m}\n")


if __name__ == "__main__":
    sys.exit(main())
