#!/usr/bin/env python3
"""Generates the Bad Sector faceplate (res/BadSector.svg) and all custom
component SVGs (res/components/*.svg): black anodised plate, off-white
ShareTechMono industrial typography, orange hazard markings, cyan diagnostics.
Text is baked to paths (nanosvg drops <text>). Keep the layout constants in
sync with src/BadSector.cpp (they are mirrored there for widget positions)."""
import os, math
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

BASE = os.path.dirname(os.path.abspath(__file__))
FONT_PATH = r"C:\Program Files\VCV\Rack2Pro\res\fonts\ShareTechMono-Regular.ttf"
font = TTFont(FONT_PATH)
upem = font["head"].unitsPerEm
cmap = font.getBestCmap()
gset = font.getGlyphSet()
hmtx = font["hmtx"]

# ------------------------------------------------------------ palette ----
BG = "#0e0f12"
EDGE = "#1c1e23"
INK = "#ece8dd"      # off-white
DIM = "#8f8c83"
ORANGE = "#e8641e"
CYAN = "#35d3e0"

W, H = 81.28, 128.5  # 16HP

# ------------------------------------------------------------- layout ----
KNOBS = {  # centre positions - keep in sync with src/BadSector.cpp
    "BUFFER": (15.0, 25.0), "REPEAT": (66.28, 25.0),
    "MIX": (15.0, 46.5), "MICRO": (66.28, 46.5),
    "DAMAGE": (15.0, 68.0), "CV AMT": (66.28, 68.0),
}
SEL1, SEL2 = (33.6, 68.0), (47.7, 68.0)          # square selector buttons
MODES = [(34.0, 56.0), (40.64, 56.0), (47.3, 56.0)]  # MODE / CLK / FRZ
JX = [9.0, 21.9, 34.8, 47.7, 60.6, 73.5]
CVY, GATEY, AUY = 89.0, 101.0, 114.0
AUX = [14.0, 31.7, 49.5, 67.2]

def text_path(x, y, s, h, color, anchor="middle", spacing=0.0, weight=0.0):
    scale = h / upem
    advs = [hmtx[cmap.get(ord(ch), ".notdef")][0] if ord(ch) in cmap else upem // 2 for ch in s]
    total = sum(advs) * scale + spacing * (len(s) - 1)
    x0 = x - total / 2.0 if anchor == "middle" else (x - total if anchor == "end" else x)
    pen = SVGPathPen(gset)
    penx = x0
    for ch, adv in zip(s, advs):
        gn = cmap.get(ord(ch))
        if gn:
            tpen = TransformPen(pen, (scale, 0, 0, -scale, penx, y))
            gset[gn].draw(tpen)
        penx += adv * scale + spacing
    d = pen.getCommands()
    if not d:
        return ""
    stroke = f' stroke="{color}" stroke-width="{weight}"' if weight > 0 else ""
    return f'<path d="{d}" fill="{color}"{stroke}/>'

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">']
svg.append(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
svg.append(f'<rect x="0.3" y="0.3" width="{W-0.6}" height="{H-0.6}" fill="none" stroke="{EDGE}" stroke-width="0.6"/>')

# ---- header ----
svg.append(text_path(W / 2, 6.4, "halfagiraf", 3.0, DIM, spacing=0.55))
svg.append(text_path(W / 2, 13.6, "BAD SECTOR", 6.2, INK, spacing=0.35, weight=0.22))
# CHK diagnostic cluster
svg.append(f'<circle cx="63.8" cy="9.6" r="0.8" fill="{CYAN}"/>')
svg.append(f'<path d="M 62.0 7.4 L 62.0 6.2 L 65.4 6.2" fill="none" stroke="{DIM}" stroke-width="0.25"/>')
svg.append(text_path(66.2, 10.4, "CHK", 1.9, DIM, anchor="start"))
svg.append(text_path(66.2, 12.7, "0xE7", 1.9, DIM, anchor="start"))
svg.append(text_path(66.2, 15.0, "A3", 1.9, DIM, anchor="start"))

# ---- knob markers + labels ----
for name, (x, y) in KNOBS.items():
    svg.append(f'<circle cx="{x}" cy="{y - 9.8}" r="0.7" fill="{INK}"/>')
    svg.append(text_path(x, y + 11.4, name, 2.7, INK, spacing=0.5))

# ---- hazard markings around DAMAGE ----
svg.append(f'<path d="M 29.2 59.2 L 5.6 59.2 L 5.6 78.8" fill="none" stroke="{ORANGE}" stroke-width="0.55"/>')
for i in range(6):  # diagonal hazard stripes, top-left block
    x0 = 6.6 + i * 1.55
    svg.append(f'<path d="M {x0} 63.0 L {x0 + 1.7} 59.8" stroke="{ORANGE}" stroke-width="0.75" fill="none"/>')
for i in range(3):  # dashes along the bottom
    svg.append(f'<rect x="{7.0 + i * 3.6}" y="76.6" width="2.4" height="0.7" fill="{ORANGE}"/>')
svg.append(f'<rect x="26.4" y="76.0" width="1.7" height="1.7" fill="{ORANGE}"/>')
# warning triangle
svg.append(f'<path d="M 19.4 76.0 L 21.0 79.0 L 17.8 79.0 Z" fill="none" stroke="{ORANGE}" stroke-width="0.5" stroke-linejoin="round"/>')
svg.append(f'<rect x="19.2" y="76.9" width="0.45" height="1.1" fill="{ORANGE}"/>')
svg.append(f'<rect x="19.2" y="78.3" width="0.45" height="0.4" fill="{ORANGE}"/>')

# ---- selector buttons: dots + labels ----
for (x, y), lab in [(SEL1, "DMG"), (SEL2, "CV")]:
    svg.append(text_path(x, y + 7.6, lab, 1.6, DIM, spacing=0.4))
for (x, y), lab in zip(MODES, ["MODE", "CLK", "FRZ"]):
    svg.append(text_path(x, y + 6.9, lab, 1.5, DIM, spacing=0.3))

# ---- jack labels ----
for x, lab in zip(JX, ["BUFFER", "REPEAT", "MIX", "BEND", "BREAK", "CRPT"]):
    svg.append(text_path(x, CVY - 5.6, lab, 1.6, DIM, spacing=0.15))
svg.append(text_path(1.8, CVY - 8.9, "CV", 1.5, DIM, anchor="start"))
for x, lab in zip(JX, ["BEND", "BREAK", "CRPT", "FRZ", "CLOCK", "RESET"]):
    col = INK if lab in ("CLOCK", "RESET") else DIM
    svg.append(text_path(x, GATEY - 5.6, lab, 1.6, col, spacing=0.15))
svg.append(text_path(1.8, GATEY - 8.9, "GATE", 1.5, DIM, anchor="start"))
for x, lab in zip(AUX, ["IN L", "IN R", "OUT L", "OUT R"]):
    svg.append(text_path(x, AUY - 6.4, lab, 2.1, INK, spacing=0.3))
# orange tick above outputs
svg.append(f'<path d="M 43.0 105.8 L 73.4 105.8" stroke="{ORANGE}" stroke-width="0.35" fill="none"/>')

# ---- footers ----
svg.append(f'<rect x="3.8" y="123.2" width="1.9" height="1.9" fill="none" stroke="{DIM}" stroke-width="0.3"/>')
svg.append(text_path(6.8, 124.9, "16HP v2.0", 1.7, DIM, anchor="start"))
svg.append(f'<rect x="61.8" y="123.2" width="1.9" height="1.9" fill="none" stroke="{DIM}" stroke-width="0.3"/>')
svg.append(text_path(64.8, 124.9, "BS-16 25/07", 1.7, DIM, anchor="start"))

svg.append("</svg>")
os.makedirs(os.path.join(BASE, "res", "components"), exist_ok=True)
open(os.path.join(BASE, "res", "BadSector.svg"), "w").write("\n".join(svg))

# ================================================== component SVGs =====
def write(name, size, body):
    s = f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}mm" height="{size}mm" viewBox="0 0 {size} {size}">\n'
    s += "\n".join(body) + "\n</svg>\n"
    open(os.path.join(BASE, "res", "components", name), "w").write(s)

# knob: black knurled cap, white pointer (16 mm)
k = []
c = 8.0
k.append(f'<circle cx="{c}" cy="{c}" r="7.7" fill="#08090b"/>')
for i in range(48):  # knurl ring
    a = i * math.pi * 2 / 48
    x0, y0 = c + math.cos(a) * 6.7, c + math.sin(a) * 6.7
    x1, y1 = c + math.cos(a) * 7.55, c + math.sin(a) * 7.55
    k.append(f'<path d="M {x0:.3f} {y0:.3f} L {x1:.3f} {y1:.3f}" stroke="#22242a" stroke-width="0.38"/>')
k.append(f'<circle cx="{c}" cy="{c}" r="6.55" fill="#111318"/>')
k.append(f'<circle cx="{c}" cy="{c}" r="6.55" fill="none" stroke="#26282f" stroke-width="0.25"/>')
k.append(f'<circle cx="{c}" cy="{c}" r="4.9" fill="#16181d"/>')
k.append(f'<rect x="{c - 0.5}" y="1.7" width="1.0" height="5.4" rx="0.5" fill="{INK}"/>')
write("knob.svg", 16, k)

# screw: black torx (5 mm)
s5 = []
s5.append('<circle cx="2.5" cy="2.5" r="2.4" fill="#101114"/>')
s5.append('<circle cx="2.5" cy="2.5" r="2.4" fill="none" stroke="#2a2d33" stroke-width="0.25"/>')
pts = []
for i in range(12):
    a = i * math.pi / 6
    r = 1.35 if i % 2 == 0 else 0.62
    pts.append(f'{2.5 + math.cos(a) * r:.3f} {2.5 + math.sin(a) * r:.3f}')
s5.append(f'<path d="M {" L ".join(pts)} Z" fill="none" stroke="#3a3d45" stroke-width="0.3" stroke-linejoin="round"/>')
write("screw.svg", 5, s5)

# port: silver knurled bezel (9.8 mm)
p = []
pc = 4.9
p.append(f'<circle cx="{pc}" cy="{pc}" r="4.8" fill="#96999f"/>')
for i in range(40):
    a = i * math.pi * 2 / 40
    x0, y0 = pc + math.cos(a) * 4.1, pc + math.sin(a) * 4.1
    x1, y1 = pc + math.cos(a) * 4.75, pc + math.sin(a) * 4.75
    p.append(f'<path d="M {x0:.3f} {y0:.3f} L {x1:.3f} {y1:.3f}" stroke="#6c6f75" stroke-width="0.3"/>')
p.append(f'<circle cx="{pc}" cy="{pc}" r="3.85" fill="#c4c7cc"/>')
p.append(f'<circle cx="{pc}" cy="{pc}" r="3.1" fill="#54575c"/>')
p.append(f'<circle cx="{pc}" cy="{pc}" r="2.55" fill="#060709"/>')
write("port.svg", 9.8, p)

# square LED button, unpressed / pressed (7.5 mm)
for frame, inset in (("sqbtn_0.svg", 0.0), ("sqbtn_1.svg", 0.28)):
    b = []
    b.append('<rect x="0.15" y="0.15" width="7.2" height="7.2" rx="0.9" fill="#0a0b0e"/>')
    b.append('<rect x="0.15" y="0.15" width="7.2" height="7.2" rx="0.9" fill="none" stroke="#26282f" stroke-width="0.3"/>')
    i0 = 1.25 + inset
    sz = 5.0 - inset * 2
    b.append(f'<rect x="{i0}" y="{i0}" width="{sz}" height="{sz}" rx="0.5" fill="#191b20"/>')
    write(frame, 7.5, b)

# small square LED button, unpressed / pressed (5.8 mm)
for frame, inset in (("sqbtn_s0.svg", 0.0), ("sqbtn_s1.svg", 0.22)):
    b = []
    b.append('<rect x="0.12" y="0.12" width="5.56" height="5.56" rx="0.7" fill="#0a0b0e"/>')
    b.append('<rect x="0.12" y="0.12" width="5.56" height="5.56" rx="0.7" fill="none" stroke="#26282f" stroke-width="0.28"/>')
    i0 = 1.0 + inset
    sz = 3.8 - inset * 2
    b.append(f'<rect x="{i0}" y="{i0}" width="{sz}" height="{sz}" rx="0.4" fill="#191b20"/>')
    write(frame, 5.8, b)

print("wrote faceplate + components")
