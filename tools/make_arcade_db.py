#!/usr/bin/env python3
"""Build the arcade ROM databases Retro Launcher uses to recognise arcade zips,
pick the emulator for each, and name and orient the games.

Usage:  make_arcade_db.py <cores-src folder> <out folder>
        e.g. make_arcade_db.py "$SDK/cores-src" /Volumes/USB/external/retro-launcher/cores

Reads the ROM lists that ship with the core sources, so a database always
matches the core built from the same checkout:
  FBNeo/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat -> fbneo_libretro.db
  mame2003-plus-libretro/metadata/mame2003-plus.xml           -> mame2003_plus_libretro.db

Output: one tab-separated line per game:
  name  cloneof  romof  flags  year  manufacturer  description  crc crc ...
flags: V vertical, B BIOS / not runnable, P preliminary (not working), C needs
a CHD (hard disk / CD image; not supported). The CRCs are every ROM the game
needs, including those that live in its parent or BIOS zip, except no-dumps.
"""
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

SOURCES = [
    ("fbneo_libretro.db", "FBNeo", "dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat"),
    ("mame2003_plus_libretro.db", "mame2003-plus-libretro", "metadata/mame2003-plus.xml"),
]


def clean(s):
    return " ".join((s or "").replace("\t", " ").split())


def build(xml_path, out_path, label):
    games = 0
    with open(out_path, "w", encoding="utf-8") as out:
        out.write(f"# retro-launcher arcade db v1\t{label}\n")
        for _, g in ET.iterparse(xml_path, events=("end",)):
            if g.tag not in ("game", "machine"):
                continue
            flags = ""
            video = g.find("video")
            if video is not None and video.get("orientation") == "vertical":
                flags += "V"
            if g.get("isbios") == "yes" or g.get("runnable") == "no" or g.get("isdevice") == "yes":
                flags += "B"
            driver = g.find("driver")
            if driver is not None and driver.get("status") == "preliminary":
                flags += "P"
            if g.find("disk") is not None:
                flags += "C"
            crcs = []
            for r in g.findall("rom"):
                if r.get("status") == "nodump" or not r.get("crc"):
                    continue
                crcs.append(r.get("crc").lower().zfill(8))
            out.write("\t".join([g.get("name"), g.get("cloneof") or "", g.get("romof") or "", flags,
                                 clean(g.findtext("year")), clean(g.findtext("manufacturer")),
                                 clean(g.findtext("description")), " ".join(sorted(set(crcs)))]) + "\n")
            games += 1
            g.clear()
    return games


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name, repo, rel in SOURCES:
        path = os.path.join(src, repo, rel)
        if not os.path.exists(path):
            print(f"{name}: {path} not found, skipped")
            continue
        commit = subprocess.run(["git", "-C", os.path.join(src, repo), "log", "-1", "--format=%h"],
                                capture_output=True, text=True).stdout.strip()
        n = build(path, os.path.join(out, name), f"{repo} @ {commit}")
        print(f"{name}: {n} games ({repo} @ {commit}), {os.path.getsize(os.path.join(out, name)) // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
