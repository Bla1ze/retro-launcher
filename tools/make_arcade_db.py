#!/usr/bin/env python3
"""Build the arcade ROM databases Retro Launcher uses to recognize arcade zips,
pick the emulator for each, and name and orient the games.

Usage:  make_arcade_db.py <cores-src folder> <out folder>
        e.g. make_arcade_db.py "$SDK/cores-src" /Volumes/USB/external/retro-launcher/cores

Reads the ROM lists that ship with the core sources, so a database always
matches the core built from the same checkout:
  FBNeo/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat -> fbneo_libretro.db
  mame2003-plus-libretro/metadata/mame2003-plus.xml           -> mame2003_plus_libretro.db
  flycast/core/hw/naomi/naomi_roms.cpp (NAOMI, Atomiswave)    -> flycast_libretro.db
  mame2003-plus-libretro/metadata/catver.ini (genres)          -> arcade-genres.txt

Output: one tab-separated line per game:
  name  cloneof  romof  flags  year  manufacturer  description  crc crc ...
flags: V vertical, B BIOS / not runnable, P preliminary (not working), C needs
a CHD (hard disk / CD image; not supported). The CRCs are every ROM the game
needs, including those that live in its parent or BIOS zip, except no-dumps.
"""
import os
import re
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


# ---- Flycast: its NAOMI / Atomiswave game list is C source (core/hw/naomi/naomi_roms.cpp).

def _c_tokens(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    src = re.sub(r"(?m)^\s*#.*$", " ", src)  # #ifdef blocks: keep what they enclose
    return re.findall(r'"(?:[^"\\]|\\.)*"|[{},]|[^\s{},"]+', src)


def _entries(tokens, table):
    """The top-level { ... } entries of `const X table[] = { ... };`, each as a
    list of fields split on depth-1 commas (a field is a token list)."""
    i = tokens.index(table + "[]")
    while tokens[i] != "{":
        i += 1
    i += 1
    out = []
    while i < len(tokens) and tokens[i] != "}":
        if tokens[i] == ",":
            i += 1
            continue
        assert tokens[i] == "{", tokens[i:i + 5]
        depth, fields, cur = 0, [], []
        while True:
            t = tokens[i]
            if t == "{":
                depth += 1
                if depth > 1:
                    cur.append(t)
            elif t == "}":
                depth -= 1
                if depth == 0:
                    fields.append(cur)
                    i += 1
                    break
                cur.append(t)
            elif t == "," and depth == 1:
                fields.append(cur)
                cur = []
            else:
                cur.append(t)
            i += 1
        out.append(fields)
    return out


def _str(field):
    return field[0][1:-1] if field and field[0].startswith('"') else ""


def _blob_crcs(field, crc_index):
    """CRCs from a field like { {"a", 0, 0x10, 0xcrc}, ... }."""
    crcs, cur, depth = [], [], 0
    for t in field:
        if t == "{":
            depth += 1
            cur = []
        elif t == "}":
            depth -= 1
            parts = [x for x in cur if x != ","]
            if len(parts) > crc_index and parts[crc_index].lower().startswith("0x"):
                c = int(parts[crc_index], 16)
                if c:
                    crcs.append("%08x" % c)
        else:
            cur.append(t)
    return sorted(set(crcs))


def build_flycast(cpp_path, out_path, label):
    tokens = _c_tokens(open(cpp_path, encoding="utf-8", errors="replace").read())
    n = 0
    with open(out_path, "w", encoding="utf-8") as out:
        out.write(f"# retro-launcher arcade db v1\t{label}\n")
        for f in _entries(tokens, "BIOS"):
            if len(f) < 2 or not _str(f[0]):
                continue  # the list ends with an empty entry
            # { name, { {region, "file", offset, length, crc}, ... }, filename }
            out.write("\t".join([_str(f[0]), "", "", "B", "", "Sega", _str(f[0]) + " BIOS",
                                  " ".join(_blob_crcs(f[1], 4))]) + "\n")
            n += 1
        for f in _entries(tokens, "Games"):
            # { name, parent, description, size, key, bios, cart, rotation, {blobs}, gdrom, ... }
            if len(f) < 9 or not _str(f[0]):
                continue
            flags = ("V" if f[7] and f[7][0] in ("ROT90", "ROT270") else "")
            if len(f) > 9 and _str(f[9]):
                flags += "C"  # GD-ROM game: needs its .chd as well
            out.write("\t".join([_str(f[0]), _str(f[1]), _str(f[5]) or "naomi", flags, "", "",
                                  " ".join(_str(f[2]).split()), " ".join(_blob_crcs(f[8], 3))]) + "\n")
            n += 1
    return n


def build_genres(catver_path, out_path):
    """catver.ini -> "<set>\t<genre>" with the top-level genre ("Shooter / Flying
    Vertical" -> "Shooter"); adult sets are marked "<genre>\tmature"."""
    n = 0
    with open(catver_path, encoding="latin-1") as src, open(out_path, "w", encoding="utf-8") as out:
        section = ""
        for line in src:
            line = line.strip()
            if line.startswith("["):
                section = line
                continue
            if section.lower() != "[category]" or "=" not in line:
                continue
            name, cat = line.split("=", 1)
            mature = "* Mature *" in cat
            top = cat.replace("* Mature *", "").split(" / ")[0].strip()
            if top:
                out.write(f"{name.strip()}\t{top}" + ("\tmature" if mature else "") + "\n")
                n += 1
    return n


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    fly = os.path.join(src, "flycast", "core", "hw", "naomi", "naomi_roms.cpp")
    if os.path.exists(fly):
        commit = subprocess.run(["git", "-C", os.path.join(src, "flycast"), "log", "-1", "--format=%h"],
                                capture_output=True, text=True).stdout.strip()
        n = build_flycast(fly, os.path.join(out, "flycast_libretro.db"), f"flycast @ {commit}")
        print(f"flycast_libretro.db: {n} sets (flycast @ {commit})")
    cat = os.path.join(src, "mame2003-plus-libretro", "metadata", "catver.ini")
    if os.path.exists(cat):
        print(f"arcade-genres.txt: {build_genres(cat, os.path.join(out, 'arcade-genres.txt'))} sets")
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
