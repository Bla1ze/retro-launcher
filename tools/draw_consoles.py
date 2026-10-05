#!/usr/bin/env python3
"""Draws the console pictures for systems that have no good photo: an arcade
cabinet (Arcade), the NAOMI board and the Atomiswave board. Writes assets/consoles/<sys>.svg
and, with Google Chrome installed, renders each to a transparent
assets/consoles/<sys>.png (what the launcher shows; fetch_media.sh copies them
into media/<sys>/console.png).

These drawings are original work for Retro Launcher, under the repository's license.
Usage: tools/draw_consoles.py
"""
import os
import random
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "consoles")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


def poly(P, pts, fill, extra=""):
    return '<polygon points="%s" fill="%s" %s/>' % (" ".join("%.1f,%.1f" % P(*p) for p in pts), fill, extra)


# ---------------------------------------------------------------- arcade cabinet

def arcade():
    # Oblique projection: (x, y, d) -> (x + 200 d, y - 80 d); d = depth (0 front, 1 back).
    def P(x, y, d):
        return (x + 200 * d, y - 80 * d)
    W = 400
    prof = [(40, .05), (40, 1), (1160, 1), (1160, .08), (760, .08), (720, -.12), (690, -.12), (640, .12),
            (330, .22), (300, .05)]
    o = ['''<svg xmlns="http://www.w3.org/2000/svg" viewBox="-60 -110 720 1300" width="1440" height="2600">
<defs>
 <linearGradient id="side" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2a1757"/><stop offset="1" stop-color="#120a2b"/></linearGradient>
 <linearGradient id="front" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#231447"/><stop offset="1" stop-color="#140c2c"/></linearGradient>
 <linearGradient id="marq" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ff3ea5"/><stop offset=".5" stop-color="#a855f7"/><stop offset="1" stop-color="#22d3ee"/></linearGradient>
 <radialGradient id="scr" cx=".5" cy=".45" r=".7"><stop offset="0" stop-color="#1b2a6b"/><stop offset="1" stop-color="#05081c"/></radialGradient>
 <linearGradient id="cp" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3b2a78"/><stop offset="1" stop-color="#2a1d5c"/></linearGradient>
 <radialGradient id="ball" cx=".35" cy=".3" r=".7"><stop offset="0" stop-color="#ff8fb0"/><stop offset=".5" stop-color="#ef2b5b"/><stop offset="1" stop-color="#8a0f2e"/></radialGradient>
 <filter id="glow" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="6" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>
</defs>''']
    # Side panel with neon side art, top, lit marquee.
    o.append(poly(P, [(W, y, d) for y, d in prof], "url(#side)", 'stroke="#0b0618" stroke-width="3"'))
    o.append(poly(P, [(W, 1160, .30), (W, 1160, .55), (W, 420, 1.0), (W, 300, 1.0)], "#ff3ea5", 'opacity=".85" filter="url(#glow)"'))
    o.append(poly(P, [(W, 1160, .62), (W, 1160, .72), (W, 560, 1.0), (W, 500, 1.0)], "#22d3ee", 'opacity=".85" filter="url(#glow)"'))
    o.append(poly(P, [(W, y, d) for y, d in prof], "none", 'stroke="#a855f7" stroke-width="4" opacity=".6"'))
    o.append(poly(P, [(0, 40, .05), (W, 40, .05), (W, 40, 1), (0, 40, 1)], "#1d1240", 'stroke="#0b0618" stroke-width="3"'))
    o.append(poly(P, [(0, 40, .05), (W, 40, .05), (W, 300, .05), (0, 300, .05)], "#160d33", 'stroke="#0b0618" stroke-width="3"'))
    o.append(poly(P, [(18, 62, .05), (W - 18, 62, .05), (W - 18, 280, .05), (18, 280, .05)], "url(#marq)", 'filter="url(#glow)"'))
    for cx, cy, r in [(80, 120, 10), (330, 110, 8), (110, 235, 7), (300, 240, 11), (205, 95, 6)]:  # sparkles
        x, y = P(cx, cy, .05)
        o.append('<path d="M%.1f %.1f l%.1f %.1f l%.1f %.1f l%.1f %.1f l%.1f %.1f l%.1f %.1f l%.1f %.1f l%.1f %.1f z" fill="#fff" opacity=".9"/>' % (
            x, y - r, r * .3, r * .7, r * .7, r * .3, -r * .7, r * .3, -r * .3, r * .7, -r * .3, -r * .7, -r * .7, -r * .3, r * .7, -r * .3))
    x0, y0 = P(130, 170, .05)
    x1, _ = P(270, 170, .05)
    o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="44" rx="22" fill="#fff" opacity=".92"/>' % (x0, y0 - 22, x1 - x0))
    o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="20" rx="10" fill="#7c3aed"/>' % (x0 + 22, y0 - 10, x1 - x0 - 44))
    # Speaker strip.
    o.append(poly(P, [(0, 300, .05), (W, 300, .05), (W, 330, .22), (0, 330, .22)], "#0d0820"))
    for i in range(9):
        o.append(poly(P, [(70 + i * 30, 307, .09), (84 + i * 30, 307, .09), (84 + i * 30, 323, .18), (70 + i * 30, 323, .18)], "#2b1d57"))
    # Screen: bezel, glass, stars, saucers and a ship (original pixel art).
    o.append(poly(P, [(0, 330, .22), (W, 330, .22), (W, 640, .12), (0, 640, .12)], "#0b0718", 'stroke="#000" stroke-width="3"'))
    o.append(poly(P, [(34, 352, .21), (W - 34, 352, .21), (W - 34, 618, .13), (34, 618, .13)], "url(#scr)", 'stroke="#22d3ee" stroke-width="3"'))
    rnd = random.Random(7)
    for _ in range(40):
        u, v = rnd.uniform(.05, .95), rnd.uniform(.05, .95)
        x, y = P(34 + u * (W - 68), 352 + v * 266, .21 - v * .08)
        o.append('<rect x="%.1f" y="%.1f" width="3" height="3" fill="#cfe8ff" opacity="%.2f"/>' % (x, y, rnd.uniform(.4, 1)))

    def sprite(rows, u, v, px, color):
        s = []
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch == "#":
                    x, y = P(34 + u * (W - 68) + i * px, 352 + v * 266 + j * px, .21 - (v + j * px / 266) * .08)
                    s.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s"/>' % (x, y, px + .5, px + .5, color))
        return "".join(s)
    saucer = ["...###...", ".#######.", "##.#.#.##", "#########", "..#...#.."]
    ship = ["....#....", "...###...", "...###...", ".#######.", "#########", "##.###.##"]
    o.append('<g filter="url(#glow)">')
    for k, (u, c) in enumerate([(.12, "#ff3ea5"), (.40, "#a855f7"), (.68, "#ff3ea5")]):
        o.append(sprite(saucer, u, .14 + (k % 2) * .08, 7, c))
    o.append(sprite(ship, .42, .74, 8, "#22d3ee"))
    x, y = P(34 + .5 * (W - 68), 352 + .52 * 266, .17)
    o.append('<rect x="%.1f" y="%.1f" width="5" height="22" fill="#fde047"/>' % (x, y))
    o.append('</g>')
    # Control panel: top, neon lip, underside; stick and six buttons.
    o.append(poly(P, [(0, 640, .12), (W, 640, .12), (W, 690, -.12), (0, 690, -.12)], "url(#cp)", 'stroke="#0b0618" stroke-width="3"'))
    o.append(poly(P, [(0, 690, -.12), (W, 690, -.12), (W, 720, -.12), (0, 720, -.12)], "#1a1038"))
    o.append(poly(P, [(0, 690, -.12), (W, 690, -.12), (W, 694, -.12), (0, 694, -.12)], "#22d3ee", 'filter="url(#glow)"'))
    o.append(poly(P, [(0, 720, -.12), (W, 720, -.12), (W, 760, .08), (0, 760, .08)], "#0f0924"))
    bx, by = P(95, 668, 0)
    o.append('<ellipse cx="%.1f" cy="%.1f" rx="26" ry="9" fill="#120a28"/>' % (bx, by))
    o.append('<rect x="%.1f" y="%.1f" width="8" height="46" rx="3" fill="#c9c9d6"/>' % (bx - 4, by - 44))
    o.append('<circle cx="%.1f" cy="%.1f" r="20" fill="url(#ball)"/>' % (bx, by - 52))
    cols = ["#ff3ea5", "#22d3ee", "#fde047", "#ff3ea5", "#22d3ee", "#fde047"]
    for i in range(6):
        cx, cy = 185 + (i % 3) * 52 + (i // 3) * 10, 658 + (i // 3) * 22
        x, y = P(cx, cy, .12 - (cy - 640) / 50 * .24)
        o.append('<ellipse cx="%.1f" cy="%.1f" rx="17" ry="7" fill="#0d0820"/>' % (x, y + 3))
        o.append('<ellipse cx="%.1f" cy="%.1f" rx="15" ry="6" fill="%s" filter="url(#glow)"/>' % (x, y - 1, cols[i]))
    # Lower body with the coin door.
    o.append(poly(P, [(0, 760, .08), (W, 760, .08), (W, 1160, .08), (0, 1160, .08)], "url(#front)", 'stroke="#0b0618" stroke-width="3"'))
    o.append(poly(P, [(120, 840, .08), (280, 840, .08), (280, 1040, .08), (120, 1040, .08)], "#2b1d57", 'stroke="#4c3a8f" stroke-width="3"'))
    for cx in (165, 235):
        o.append(poly(P, [(cx - 22, 870, .08), (cx + 22, 870, .08), (cx + 22, 930, .08), (cx - 22, 930, .08)], "#140c2c"))
        o.append(poly(P, [(cx - 3, 880, .08), (cx + 3, 880, .08), (cx + 3, 920, .08), (cx - 3, 920, .08)], "#ff8a1f", 'filter="url(#glow)"'))
        o.append(poly(P, [(cx - 18, 950, .08), (cx + 18, 950, .08), (cx + 18, 985, .08), (cx - 18, 985, .08)], "#ef2b5b", 'filter="url(#glow)"'))
    o.append(poly(P, [(0, 1120, .08), (W, 1120, .08), (W, 1160, .08), (0, 1160, .08)], "#0b0618"))
    o.append('</svg>')
    return "\n".join(o), (1440, 2600)


# ---------------------------------------------------------------- NAOMI board

def naomi():
    # Oblique projection: (x, z, d) -> (x + .62 d, -z - .42 d); z = height, d = depth.
    def P(x, z, d):
        return (x + .62 * d, -z - .42 * d)

    def box(x0, x1, d0, d1, z0, z1, top, front, side, stroke="#0a1230"):
        s = 'stroke="%s" stroke-width="2"' % stroke
        return [poly(P, [(x0, z1, d0), (x1, z1, d0), (x1, z1, d1), (x0, z1, d1)], top, s),
                poly(P, [(x0, z0, d0), (x1, z0, d0), (x1, z1, d0), (x0, z1, d0)], front, s),
                poly(P, [(x1, z0, d0), (x1, z0, d1), (x1, z1, d1), (x1, z1, d0)], side, s)]
    o = ['''<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -330 1010 370" width="2020" height="740">
<defs>
 <linearGradient id="top" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#34508f"/><stop offset="1" stop-color="#22366b"/></linearGradient>
 <linearGradient id="front" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1d2c5e"/><stop offset="1" stop-color="#141f45"/></linearGradient>
 <linearGradient id="side" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#16224b"/><stop offset="1" stop-color="#0f1838"/></linearGradient>
 <linearGradient id="pcb" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#1f7a4a"/><stop offset="1" stop-color="#145a35"/></linearGradient>
</defs>''']
    # Filter board (green PCB with its connectors) on the left side.
    o += box(-60, 0, 30, 330, 0, 70, "url(#pcb)", "#176a3f", "#125232", "#0b3a22")
    for d0, d1 in [(60, 100), (140, 210), (240, 300)]:
        o += box(-56, -14, d0, d1, 70, 92, "#3a3f4a", "#2a2e37", "#22262e", "#15171c")
    # The case, its raised module at the back left, and vents.
    o += box(0, 620, 0, 420, 0, 96, "url(#top)", "url(#front)", "url(#side)")
    o += box(20, 330, 210, 410, 96, 128, "#3b5aa0", "#24376f", "#1a2a58")
    for i in range(8):
        x = 45 + i * 34
        o.append(poly(P, [(x, 128, 240), (x + 14, 128, 240), (x + 14, 128, 385), (x, 128, 385)], "#15224a"))
    for j in range(5):
        for i in range(7):
            x, d = 360 + i * 34, 60 + j * 62
            o.append(poly(P, [(x, 96, d), (x + 22, 96, d), (x + 22, 96, d + 34), (x, 96, d + 34)], "#1a2a5a"))
    # Front: the label with the NAOMI wordmark, connectors, a status LED.
    lx, ly = P(40, 76, 0)
    o.append('<rect x="%.1f" y="%.1f" width="210" height="52" rx="4" fill="#f4f4f0" stroke="#c9c9c2" stroke-width="2"/>' % (lx, ly))
    o.append('<text x="%.1f" y="%.1f" font-family="Helvetica Neue, Arial, sans-serif" font-size="38" font-weight="700" '
             'letter-spacing="5" fill="#111">NA<tspan fill="#f26522">O</tspan>MI</text>' % (lx + 22, ly + 41))
    for x0, x1 in [(300, 380), (400, 450), (470, 590)]:
        a, b = P(x0, 70, 0), P(x1, 26, 0)
        o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="3" fill="#2a2e37" stroke="#11131a" stroke-width="2"/>' % (
            a[0], a[1], b[0] - a[0], b[1] - a[1]))
        for k in range(int((x1 - x0) / 12)):
            o.append('<rect x="%.1f" y="%.1f" width="5" height="%.1f" fill="#c9a447"/>' % (a[0] + 6 + k * 12, a[1] + 10, b[1] - a[1] - 20))
    x, y = P(272, 56, 0)
    o.append('<circle cx="%.1f" cy="%.1f" r="6" fill="#4ade80"/>' % (x, y))
    o.append('</svg>')
    return "\n".join(o), (2020, 740)


# ---------------------------------------------------------------- Atomiswave board

def atomiswave():
    # Same projection as the NAOMI board: (x, z, d) -> (x + .62 d, -z - .42 d).
    def P(x, z, d):
        return (x + .62 * d, -z - .42 * d)

    def box(x0, x1, d0, d1, z0, z1, top, front, side, stroke="#5a1d06"):
        s = 'stroke="%s" stroke-width="2"' % stroke
        return [poly(P, [(x0, z1, d0), (x1, z1, d0), (x1, z1, d1), (x0, z1, d1)], top, s),
                poly(P, [(x0, z0, d0), (x1, z0, d0), (x1, z1, d0), (x0, z1, d0)], front, s),
                poly(P, [(x1, z0, d0), (x1, z0, d1), (x1, z1, d1), (x1, z1, d0)], side, s)]
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="-100 -400 1080 430" width="2160" height="860">',
         '<defs>',
         ' <linearGradient id="atop" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ff8a3d"/><stop offset="1" stop-color="#f0641f"/></linearGradient>',
         ' <linearGradient id="afront" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e2561a"/><stop offset="1" stop-color="#c44812"/></linearGradient>',
         ' <linearGradient id="aside" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#b8420f"/><stop offset="1" stop-color="#9c360b"/></linearGradient>',
         ' <linearGradient id="plate" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#d9dde3"/><stop offset="1" stop-color="#a9afb8"/></linearGradient>',
         ' <linearGradient id="cart" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ff4b4b"/><stop offset="1" stop-color="#c81e2a"/></linearGradient>',
         '</defs>']
    # Metal mounting plate, wider than the case, with screw holes.
    o += box(-70, 690, -20, 440, -8, 0, "url(#plate)", "#9aa0a8", "#868c94", "#6b7078")
    for x, d in [(-45, 10), (-45, 410), (665, 10), (665, 410)]:
        cx, cy = P(x, 0, d)
        o.append('<ellipse cx="%.1f" cy="%.1f" rx="9" ry="5" fill="#5d636b"/>' % (cx, cy))
    # The case, with the cartridge bay raised around the slot.
    o += box(0, 620, 0, 420, 0, 100, "url(#atop)", "url(#afront)", "url(#aside)")
    o += box(150, 560, 90, 400, 100, 122, "#ff9a52", "#e8601f", "#c24a12")
    o.append(poly(P, [(190, 122, 130), (520, 122, 130), (520, 122, 370), (190, 122, 370)], "#8f2f08"))
    # The cartridge, standing in its slot, with a plain label.
    o += box(205, 505, 200, 250, 110, 280, "url(#cart)", "#d42a32", "#a51d25", "#6e0f16")
    la, lb = P(225, 262, 200), P(485, 168, 200)
    o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="6" fill="#f6f4ee"/>' % (la[0], la[1], lb[0] - la[0], lb[1] - la[1]))
    o.append('<text x="%.1f" y="%.1f" text-anchor="middle" font-family="Helvetica Neue, Arial, sans-serif" font-size="30" '
             'font-weight="800" letter-spacing="2" fill="#222">ATOMISWAVE</text>' % ((la[0] + lb[0]) / 2, (la[1] + lb[1]) / 2 + 10))
    s1, s2 = P(225, 150, 200), P(485, 150, 200)
    o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="10" fill="#e23b3b"/>' % (s1[0], s1[1], s2[0] - s1[0]))
    # Edge connector (gold fingers) at the front left, below the vents.
    o += box(10, 240, -24, 0, 6, 26, "#1f6b3f", "#17592f", "#124726", "#0b3a22")
    a, b = P(18, 24, -24), P(232, 8, -24)
    for k in range(18):
        o.append('<rect x="%.1f" y="%.1f" width="6" height="%.1f" fill="#d8b24a"/>' % (a[0] + k * 12, a[1] + 2, b[1] - a[1] - 4))
    # Front: vents and the connectors.
    for i in range(10):
        a, b = P(280 + i * 22, 82, 0), P(292 + i * 22, 30, 0)
        o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="3" fill="#9c3a0e"/>' % (a[0], a[1], b[0] - a[0], b[1] - a[1]))
    for x0, x1 in [(510, 560), (570, 605)]:
        a, b = P(x0, 72, 0), P(x1, 34, 0)
        o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="4" fill="#2a2e37" stroke="#11131a" stroke-width="2"/>' % (
            a[0], a[1], b[0] - a[0], b[1] - a[1]))
    o.append('</svg>')
    return "\n".join(o), (2160, 860)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, (svg, (w, h)) in [("arcade", arcade()), ("naomi", naomi()), ("atomiswave", atomiswave())]:
        path = os.path.join(OUT, name + ".svg")
        with open(path, "w") as f:
            f.write(svg)
        if os.path.exists(CHROME):
            subprocess.run([CHROME, "--headless", "--disable-gpu", "--hide-scrollbars", "--default-background-color=00000000",
                            "--window-size=%d,%d" % (w, h), "--screenshot=" + os.path.join(OUT, name + ".png"),
                            "file://" + os.path.abspath(path)], capture_output=True, check=True)
        print(name, "done")


if __name__ == "__main__":
    main()
