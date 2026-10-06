#!/usr/bin/env python3
"""Builds the website's guides (docs/wiki/) from site/wiki/.

Each guide is an HTML fragment in site/wiki/<slug>.html, starting with:

    <!-- title: Installing & updating -->
    <!-- lede: One sentence under the title. -->
    <!-- order: 1 -->

followed by its content (<h2>, <p>, <table>...). This script wraps every page in
the shared layout (top bar, the list of guides, the page's contents, previous /
next links), adds ids to <h2>s for the contents list, and renders the Systems
guide from the SYSTEMS table below, so its 21 sections stay alike.

Usage: tools/build_wiki.py      (then commit docs/wiki/)
"""
import html
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "site", "wiki")
OUT = os.path.join(ROOT, "docs", "wiki")

# ------------------------------------------------------------------ systems
# id, name, files, emulator, gpu, BIOS [(file, folder, required, note)], buttons, notes.
# Buttons are the cabinet's -> the console's, as the app's playfield card shows them.
SYSTEMS = [
    ("arcade", "Arcade", ".zip (as they are, never unzipped)", "FBNeo or MAME 2003-Plus, picked per game", False,
     [("BIOS / parent zips", "roms/arcade/", True, "only the games that need one, e.g. neogeo.zip, pgm.zip, or a clone's parent")],
     "A=Button 1, B=Button 2, X=Button 3, Y=Button 4, LB=Button 5, RB=Button 6, Rewind=Coin, Start=Start",
     "Each zip is checked against both emulators' ROM lists; see the <a href=\"arcade.html\">Arcade guide</a>. Vertical games go to the playfield."),
    ("neogeo", "Neo Geo", ".zip (FBNeo sets, e.g. mslug.zip)", "FBNeo", False,
     [("neogeo.zip", "roms/neogeo/ or system/fbneo/", True, "one in roms/arcade/ is copied over for you")],
     "A=A, B=B, X=C, Y=D, Rewind=Coin, Start=Start",
     "Zips that aren't Neo Geo games are listed with a note to move them to roms/arcade/. Neo Geo games in roms/arcade/ play there too."),
    ("naomi", "NAOMI", ".zip (MAME-style sets); GD-ROM games also need their .chd in a folder named after the zip", "Flycast", True,
     [("naomi.zip", "roms/naomi/ or system/dc/", True, "some games want their own, e.g. hod2bios.zip")],
     "A=Button 1, B=Button 2, X=Button 3, Y=Button 4, LB=Button 5, RB=Button 6, Rewind=Coin, Start=Start",
     "Example GD-ROM layout: <code>roms/naomi/ikaruga.zip</code> + <code>roms/naomi/ikaruga/gdl-0010.chd</code>."),
    ("atomiswave", "Atomiswave", ".zip (MAME-style sets)", "Flycast", True,
     [("awbios.zip", "roms/atomiswave/ or system/dc/", True, "")],
     "A=Button 1, B=Button 2, X=Button 3, Y=Button 4, LB=Button 5, RB=Button 6, Rewind=Coin, Start=Start", ""),
    ("nes", "NES", ".nes, .zip", "FCEUmm (the firmware's QuickNES if FCEUmm is missing)", False, [],
     "A=B, B=A, Start=Start, Rewind=Select", ""),
    ("snes", "Super Nintendo", ".sfc, .smc, .zip", "Snes9x (the firmware's SNES Faust if Snes9x is missing)", False, [],
     "A=B, B=A, X=Y, Y=X, LB=L, RB=R, Start=Start, Rewind=Select",
     "Squashed file names like <code>supermarioworld.zip</code> are shown with their real title when a cover matches."),
    ("n64", "Nintendo 64", ".z64, .n64, .v64, .zip", "Mupen64Plus-Next", True, [],
     "A=A, X=B, LB2=Z, Rewind=L, RB2=R, Y=C-Up, B=C-Down, LB=C-Left, RB=C-Right, Start=Start",
     "The C buttons have buttons of their own (the cabinet has no right stick). Renders at 2x (1280x960) by default."),
    ("genesis", "Genesis / Mega Drive", ".md, .gen, .smd, .bin, .zip", "Genesis Plus GX (the firmware's)", False, [],
     "X=A, A=B, B=C, LB=X, Y=Y, RB=Z, Start=Start, Rewind=Mode", ""),
    ("mastersystem", "Master System", ".sms, .zip", "Genesis Plus GX (the firmware's)", False, [],
     "A=Button 1, B=Button 2, Start=Pause", ""),
    ("saturn", "Saturn", ".chd, .cue, .iso, .ccd", "YabaSanshiro", True,
     [("saturn_bios.bin", "system/", False, "runs more games than the built-in BIOS")],
     "A=A, B=B, LB=C, X=X, Y=Y, RB=Z, LB2=L, RB2=R, Start=Start",
     "Runs at its original resolution, with auto-frameskip on so busy scenes drop a drawn frame rather than the sound."),
    ("dreamcast", "Dreamcast", ".chd, .cdi, .gdi, .cue, .m3u", "Flycast", True,
     [("dc_boot.bin", "system/dc/", False, "needed for homebrew and some conversions; official discs run without it"),
      ("dc_flash.bin", "system/dc/", False, "goes with dc_boot.bin")],
     "A=A, B=B, X=X, Y=Y, LB2=L trigger, RB2=R trigger, Start=Start",
     "A .gdi game can be a folder of its own: <code>roms/dreamcast/Game/disc.gdi</code>. Renders at 2x by default."),
    ("psx", "PlayStation", ".chd, .cue, .pbp, .m3u, .iso, .img, .bin", "PCSX ReARMed", False,
     [("scph5501.bin", "system/", False, "USA; scph5500.bin (Japan) and scph5502.bin (Europe) work too. Runs more games than the built-in BIOS")],
     "A=Cross, B=Circle, X=Square, Y=Triangle, LB=L1, RB=R1, LB2=L2, RB2=R2, Start=Start, Rewind=Select",
     "Multi-disc games: list the discs in an .m3u and use Change disc in the pause menu."),
    ("pce", "PC Engine / TurboGrafx-16", ".pce, .sgx, .zip", "Beetle PCE Fast", False, [],
     "A=II, B=I, Start=Run, Rewind=Select", "HuCard games only; CD games aren't supported."),
    ("atari2600", "Atari 2600", ".a26, .bin, .zip", "Stella (the firmware's)", False, [],
     "A=Fire, Start=Reset, Rewind=Select", ""),
    ("colecovision", "ColecoVision", ".col, .rom, .bin, .zip", "The cabinet's own (built-in BIOS); Gearcoleco if you add a BIOS", False,
     [("colecovision.rom", "system/", False, "switches to Gearcoleco; not needed")],
     "B=Left fire, A=Right fire, Start=Keypad 1, Rewind=Keypad *, Y=on-screen keypad",
     "Auto-start presses the game number for you at the select screen (keypad 1 unless you change it with Home > Start with)."),
    ("gb", "Game Boy", ".gb, .zip", "Gambatte", False,
     [("gb_bios.bin", "system/", False, "boot logo only")], "A=B, B=A, Start=Start, Rewind=Select", ""),
    ("gbc", "Game Boy Color", ".gbc, .gb, .zip", "Gambatte", False,
     [("gbc_bios.bin", "system/", False, "boot logo only")], "A=B, B=A, Start=Start, Rewind=Select", ""),
    ("gba", "Game Boy Advance", ".gba, .zip", "gpSP", False,
     [("gba_bios.bin", "system/", False, "fixes a few games; gpSP has a built-in BIOS")],
     "A=B, B=A, LB=L, RB=R, Start=Start, Rewind=Select", ""),
    ("gamegear", "Game Gear", ".gg, .zip", "Genesis Plus GX (the firmware's)", False, [],
     "A=Button 1, B=Button 2, Start=Start", ""),
    ("lynx", "Atari Lynx", ".lnx, .zip", "Handy", False,
     [("lynxboot.img", "system/", False, "Handy starts most games without it")],
     "A=B, B=A, LB=Option 1, RB=Option 2, Start=Pause", ""),
    ("psp", "PSP", ".iso, .cso, .chd, .pbp, .elf (don't zip them)", "PPSSPP", True, [],
     "A=Cross, B=Circle, X=Square, Y=Triangle, LB=L, RB=R, Start=Start, Rewind=Select",
     ".cso (compressed .iso) or .chd saves space. Renders at 2x (960x544) by default; lighter games can go higher in Core options."),
]

GROUPS = [("Arcade", ["arcade", "neogeo", "naomi", "atomiswave"]),
          ("Consoles", ["nes", "snes", "n64", "genesis", "mastersystem", "saturn", "dreamcast", "psx", "pce", "atari2600", "colecovision"]),
          ("Handhelds", ["gb", "gbc", "gba", "gamegear", "lynx", "psp"])]


def systems_page():
    by = {s[0]: s for s in SYSTEMS}
    out = ['<h2 id="overview">At a glance</h2>',
           '<table><tr><th>System</th><th>Folder</th><th>Emulator</th><th>BIOS</th></tr>']
    for group, ids in GROUPS:
        for i in ids:
            sid, name, files, emu, gpu, bios, keys, notes = by[i]
            need = ("<span class=\"tag req\">required</span>" if any(b[2] for b in bios)
                    else "<span class=\"tag opt\">good to have</span>" if bios else "none")
            out.append(f'<tr><td><a href="#{sid}">{html.escape(name)}</a></td><td><code>roms/{sid}/</code></td>'
                       f'<td>{html.escape(emu.split(" (")[0])}{" <span class=\"tag gpu\">GPU</span>" if gpu else ""}</td><td>{need}</td></tr>')
    out.append('</table>')
    for group, ids in GROUPS:
        out.append(f'<h2 id="{group.lower()}">{group}</h2>')
        for i in ids:
            sid, name, files, emu, gpu, bios, keys, notes = by[i]
            out.append(f'<div class="sys" id="{sid}"><h3>{html.escape(name)}</h3><dl>')
            out.append(f'<dt>Folder</dt><dd><code>roms/{sid}/</code></dd>')
            out.append(f'<dt>Files</dt><dd>{html.escape(files)}</dd>')
            out.append(f'<dt>Emulator</dt><dd>{html.escape(emu)}{" <span class=\"tag gpu\">GPU</span>" if gpu else ""}</dd>')
            if bios:
                rows = []
                for f, where, req, note in bios:
                    tag = '<span class="tag req">required</span>' if req else '<span class="tag opt">good to have</span>'
                    rows.append(f'<code>{html.escape(f)}</code> in <code>{html.escape(where)}</code> {tag}'
                                + (f' &ndash; {html.escape(note)}' if note else ''))
                out.append('<dt>BIOS</dt><dd>' + '<br>'.join(rows) + '</dd>')
            else:
                out.append('<dt>BIOS</dt><dd>None needed</dd>')
            ks = ''.join(f'<span><kbd>{html.escape(k.split("=")[0])}</kbd> {html.escape(k.split("=", 1)[1])}</span>'
                         for k in keys.split(", "))
            out.append(f'<dt>Buttons</dt><dd><div class="keys">{ks}</div></dd>')
            if notes:
                out.append(f'<dt>Notes</dt><dd>{notes}</dd>')
            out.append('</dl></div>')
    return "\n".join(out)


# ------------------------------------------------------------------ layout

def meta(text, key, default=""):
    m = re.search(r"<!--\s*" + key + r":\s*(.*?)\s*-->", text)
    return m.group(1) if m else default


def slugify(t):
    return re.sub(r"[^a-z0-9]+", "-", re.sub(r"<[^>]+>", "", t).lower()).strip("-")


def add_ids(body):
    seen = set()

    def fix(m):
        attrs, text = m.group(1), m.group(2)
        if "id=" in attrs:
            return m.group(0)
        sid = slugify(text) or "section"
        while sid in seen:
            sid += "-2"
        seen.add(sid)
        return f'<h2 id="{sid}"{attrs}>{text}</h2>'
    return re.sub(r"<h2([^>]*)>(.*?)</h2>", fix, body)


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} - Retro Launcher guides</title>
<meta name="description" content="{desc}">
<link rel="icon" href="../assets/retro-launcher.png">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Bebas+Neue&family=Noto+Sans:wght@400;600&family=Press+Start+2P&display=swap" rel="stylesheet">
<link rel="stylesheet" href="../assets/wiki.css">
</head>
<body>
<!-- Generated by tools/build_wiki.py from site/wiki/{slug}.html: edit that, not this. -->
<header class="top"><div class="in">
  <a class="brand" href="../"><img src="../assets/retro-launcher.png" alt=""><span>Retro Launcher</span></a>
  <nav><a href="../">Home</a><a href="./" class="{guides_here}">Guides</a><a href="../#faq">FAQ</a>
    <a href="https://github.com/Bla1ze/retro-launcher">GitHub</a>
    <a class="dl" href="https://github.com/Bla1ze/retro-launcher/releases/latest">Download</a></nav>
</div></header>
<div class="page">
  <aside class="side">
    <h4>GUIDES</h4>
    <ul class="guides">{guides}</ul>
    {toc}
  </aside>
  <main>
    <div class="crumb">GUIDES{crumb}</div>
    <h1>{title}</h1>
    <p class="lede">{lede}</p>
{body}
    {pager}
  </main>
</div>
<footer>Retro Launcher is a free, open-source fan project (<a href="https://github.com/Bla1ze/retro-launcher/blob/main/LICENSE">GPL-3.0</a>),
not affiliated with or endorsed by AtGames. It ships no games, BIOS files or copyrighted artwork.
Something missing or wrong here? <a href="https://github.com/Bla1ze/retro-launcher/issues">Open an issue</a>.</footer>
</body>
</html>
"""


def main():
    pages = []
    for f in sorted(os.listdir(SRC)):
        if not f.endswith(".html"):
            continue
        text = open(os.path.join(SRC, f), encoding="utf-8").read()
        slug = f[:-5]
        body = re.sub(r"<!--.*?-->\s*", "", text, flags=re.S).strip()
        if slug == "systems":
            body = (body + "\n" if body else "") + systems_page()
        pages.append({"slug": slug, "title": meta(text, "title", slug), "lede": meta(text, "lede"),
                      "order": int(meta(text, "order", "99")), "body": add_ids(body)})
    pages.sort(key=lambda p: p["order"])
    os.makedirs(OUT, exist_ok=True)
    guides = [p for p in pages if p["slug"] != "index"]
    for n, p in enumerate(pages):
        href = lambda q: "./" if q["slug"] == "index" else q["slug"] + ".html"
        nav = "".join(f'<li><a href="{href(q)}"{" class=\"here\"" if q is p else ""}>{q["title"]}</a></li>' for q in guides)
        heads = re.findall(r'<h2 id="([^"]+)"[^>]*>(.*?)</h2>', p["body"])
        toc = ""
        if len(heads) > 1:
            toc = '<div class="toc-wrap"><h4>ON THIS PAGE</h4><ul class="toc">' + "".join(
                f'<li><a href="#{i}">{t}</a></li>' for i, t in heads) + "</ul></div>"
        pager = ""
        if p["slug"] != "index":
            i = guides.index(p)
            prev = guides[i - 1] if i > 0 else None
            nxt = guides[i + 1] if i + 1 < len(guides) else None
            pager = '<div class="pager">' + (
                f'<a href="{href(prev)}"><small>Previous</small>{prev["title"]}</a>' if prev else "<span></span>") + (
                f'<a href="{href(nxt)}" style="text-align:right"><small>Next</small>{nxt["title"]}</a>' if nxt else "") + "</div>"
        out = PAGE.format(title=p["title"], desc=html.escape(re.sub(r"<[^>]+>", "", p["lede"]), quote=True), slug=p["slug"],
                          guides=nav, toc=toc, body=p["body"], pager=pager, lede=p["lede"],
                          crumb="" if p["slug"] == "index" else " / " + p["title"].upper(),
                          guides_here="here" if p["slug"] == "index" else "")
        name = "index.html" if p["slug"] == "index" else p["slug"] + ".html"
        with open(os.path.join(OUT, name), "w", encoding="utf-8") as fh:
            fh.write(out)
        print("docs/wiki/" + name)


if __name__ == "__main__":
    main()
