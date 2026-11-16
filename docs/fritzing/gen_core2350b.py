"""Gera a peca Fritzing da Waveshare Core2350B (64 pinos, grade 10x10 de 2,54 mm).

Fonte da pinagem: esquematico oficial Core2350B.pdf (headers P1..P4 8x2),
conferida contra a serigrafia da imagem de dimensoes (Core2350B0-details-size.jpg).

Coordenadas: vista de cima (lado dos componentes), col 0..9 da esquerda para a
direita, row 0..9 de cima para baixo. A linha de cima (row 0) e a que tem
VB/3E/RU/U+/U-/BS na serigrafia. Unidades dos SVGs: 1/1000 pol (1000 = 1 in).
Placa 25,40 mm = 1000; pads em 50 + 100*i (margem de 1,27 mm).
"""
import os, sys, zipfile
from xml.sax.saxutils import escape

OUT = sys.argv[1]
MID = "waveshare-core2350b_1"

# ---- headers do esquematico -------------------------------------------------
P1 = ["GND", "GP0", "GP2", "GP1", "GP4", "GP3", "GP6", "GP5",
      "GP8", "GP7", "GP10", "GP9", "GP12", "GP11", "GP14", "GP13"]
P2 = ["GND", "GP45", "GP47", "GP46", "BOOTSEL", "SWDIO", "USB_DM", "GND",
      "USB_DP", "SWCLK", "RUN", "ADC_VREF", "3V3_EN", "GND", "VBUS", "3V3"]
P3 = ["GND", "GP15", "GP17", "GP16", "GP19", "GP18", "GP21", "GP20",
      "GP23", "GP22", "GP25", "GP24", "GP27", "GP26", "GP29", "GP28"]
P4 = ["GND", "GP30", "GP32", "GP31", "GP34", "GP33", "GP36", "GP35",
      "GP38", "GP37", "GP40", "GP39", "GP42", "GP41", "GP44", "GP43"]

def pos(hdr, pin):           # pin 1..16 -> (col,row)
    k, odd = (pin - 1) // 2, pin % 2 == 1   # odd = anel externo
    if hdr == "P1": return (0 if odd else 1, k)
    if hdr == "P3": return (k, 9 if odd else 8)
    if hdr == "P4": return (9 if odd else 8, 9 - k)
    if hdr == "P2": return (9 - k, 0 if odd else 1)

EXTRA = {
    "GP40": "ADC0", "GP41": "ADC1", "GP42": "ADC2", "GP43": "ADC3",
    "GP44": "ADC4", "GP45": "ADC5", "GP46": "ADC6",
    "GP47": "ADC7; PSRAM CS nas versoes com PSRAM (R11 0R)",
    "3V3": "saida 3,3 V do LDO ME6217 (800 mA max)",
    "VBUS": "entrada 5 V (USB via adaptador FPC)",
    "3V3_EN": "enable do LDO (pull-up 100k para VBUS); GND desliga o 3V3",
    "ADC_VREF": "referencia do ADC", "RUN": "reset (ativo em nivel baixo)",
    "BOOTSEL": "botao BOOT (QSPI_SS)", "SWCLK": "debug SWD", "SWDIO": "debug SWD",
    "USB_DP": "USB D+", "USB_DM": "USB D-",
}

# texto da serigrafia da placa, escrito em cada pad da vista breadboard
SILK = {"GND": "G", "VBUS": "VB", "3V3": "3V", "3V3_EN": "3E", "RUN": "RU",
        "ADC_VREF": "AV", "USB_DP": "U+", "USB_DM": "U-", "SWCLK": "SC",
        "SWDIO": "SD", "BOOTSEL": "BS"}
SILK.update({f"GP{i}": str(i) for i in range(48)})

pins = []   # (id, name, hdr, pin, col, row)
for hdr, lst in (("P1", P1), ("P2", P2), ("P3", P3), ("P4", P4)):
    for i, name in enumerate(lst, 1):
        c, r = pos(hdr, i)
        pins.append((len(pins), name, hdr, i, c, r))

cells = {(p[4], p[5]) for p in pins}
assert len(cells) == 64, "posicoes repetidas"
assert all(c in (0, 1, 8, 9) or r in (0, 1, 8, 9) for c, r in cells)
gps = sorted(int(p[1][2:]) for p in pins if p[1].startswith("GP"))
assert gps == list(range(48)), gps
gnd = [p[0] for p in pins if p[1] == "GND"]
assert len(gnd) == 6
assert all(p[1] in SILK for p in pins), "pino sem texto de serigrafia"

XY = lambda c, r: (50 + 100 * c, 50 + 100 * r)
HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" version="1.2" '
        'width="{w}in" height="{h}in" viewBox="0 0 {vw} {vh}">\n')

# ---- breadboard ---------------------------------------------------------------
def breadboard():
    s = [HEAD.format(w=1, h=1, vw=1000, vh=1000), '<g id="breadboard">\n',
         '<rect x="0" y="0" width="1000" height="1000" rx="35" fill="#1d4f9c"/>\n',
         # RP2350B QFN-80 (10 x 10 mm)
         '<rect x="215" y="330" width="394" height="394" fill="#1a1a1a" stroke="#c8c8c8" stroke-width="6"/>\n',
         '<text x="412" y="510" font-family="Droid Sans" font-size="46" fill="#9a9a9a" text-anchor="middle">RP2350B</text>\n',
         '<text x="412" y="570" font-family="Droid Sans" font-size="34" fill="#9a9a9a" text-anchor="middle">Core2350B</text>\n',
         # conector FPC 6 pinos (USB via adaptador)
         '<rect x="560" y="190" width="200" height="80" fill="#e8e8e8" stroke="#555" stroke-width="5"/>\n',
         '<rect x="575" y="235" width="170" height="25" fill="#333"/>\n',
         # LDO, caps, cristal, LED
         '<rect x="320" y="185" width="80" height="110" fill="#222"/>\n',
         '<rect x="430" y="215" width="70" height="70" fill="#222"/>\n',
         '<rect x="640" y="590" width="80" height="60" fill="#d8d8d8" stroke="#888" stroke-width="4"/>\n',
         '<rect x="655" y="455" width="45" height="22" fill="#e04040"/>\n',
         '<text x="700" y="380" font-family="Droid Sans" font-size="40" font-weight="bold" fill="#ffffff" text-anchor="middle">W</text>\n']
    for pid, name, hdr, i, c, r in pins:
        x, y = XY(c, r)
        s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{y}" r="36" fill="#d8b47a"/>\n')
        s.append(f'<text x="{x}" y="{y + 11}" font-family="Droid Sans" font-size="30" '
                 f'font-weight="bold" fill="#1a1a1a" text-anchor="middle">{escape(SILK[name])}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

# ---- esquematico -------------------------------------------------------------
LEFT = [f"GP{i}" for i in range(32)]
RIGHT = ["3V3", "VBUS", "3V3_EN", "RUN", "BOOTSEL", "ADC_VREF", "SWCLK", "SWDIO",
         "USB_DP", "USB_DM"] + [f"GP{i}" for i in range(32, 48)] + ["GND"] * 6

def schematic():
    W, Y0, N = 1400, 300, 32
    H = Y0 + (N - 1) * 100 + 200
    s = [HEAD.format(w=W / 1000, h=H / 1000, vw=W, vh=H), '<g id="schematic">\n',
         f'<rect x="100" y="150" width="{W-200}" height="{H-250}" fill="#ffffff" stroke="#000000" stroke-width="10"/>\n',
         f'<text x="{W/2}" y="110" font-family="Droid Sans" font-size="70" fill="#000000" text-anchor="middle">Waveshare Core2350B</text>\n']
    by_name = {}
    for p in pins:
        by_name.setdefault(p[1], []).append(p)
    def put(name, side, idx):
        p = by_name[name].pop(0)
        pid, _, hdr, i = p[:4]
        y = Y0 + idx * 100
        if side == "L":
            x1, x2, tx, anc, nx = 0, 100, 125, "start", 50
        else:
            x1, x2, tx, anc, nx = W - 100, W, W - 125, "end", W - 50
        tx_ = 0 if side == "L" else W - 10
        s.append(f'<line id="connector{pid}pin" x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" stroke="#555555" stroke-width="10"/>\n')
        s.append(f'<rect id="connector{pid}terminal" x="{tx_}" y="{y-5}" width="10" height="10" fill="none" stroke="none"/>\n')
        s.append(f'<text x="{tx}" y="{y+17}" font-family="Droid Sans" font-size="50" fill="#555555" text-anchor="{anc}">{escape(name)}</text>\n')
        s.append(f'<text x="{nx}" y="{y-14}" font-family="Droid Sans" font-size="30" fill="#555555" text-anchor="middle">{hdr}.{i}</text>\n')
    for k, n in enumerate(LEFT): put(n, "L", k)
    for k, n in enumerate(RIGHT): put(n, "R", k)
    assert not any(by_name.values()), by_name
    s.append('</g>\n</svg>\n')
    return "".join(s)

# ---- pcb ---------------------------------------------------------------------
def pcb():
    s = [HEAD.format(w=1, h=1, vw=1000, vh=1000), '<g id="silkscreen">\n',
         '<rect x="4" y="4" width="992" height="992" rx="30" fill="none" stroke="#000000" stroke-width="8"/>\n',
         # contorno do conector FPC (lado de cima)
         '<rect x="560" y="190" width="200" height="80" fill="none" stroke="#000000" stroke-width="8"/>\n',
         '<text x="500" y="520" font-family="OCRA" font-size="60" fill="#000000" text-anchor="middle">Core2350B</text>\n',
         '</g>\n<g id="copper1">\n<g id="copper0">\n']
    for pid, name, hdr, i, c, r in pins:
        x, y = XY(c, r)
        if name == "GND" and hdr == "P1" and i == 1:   # marca de pino 1: pad quadrado
            s.append(f'<rect id="connector{pid}pin" x="{x-30}" y="{y-30}" width="60" height="60" '
                     f'fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
        else:
            s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{y}" r="30" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
    s.append('</g>\n</g>\n</svg>\n')
    return "".join(s)

# ---- fzp ---------------------------------------------------------------------
def fzp():
    desc = ("Waveshare Core2350B (RP2350B, 16 MB flash, PSRAM opcional 0/2/8 MB). "
            "Placa 25,4 x 25,4 mm, 64 pinos em grade 2,54 mm (anel duplo de 10x10, "
            "headers P1..P4 8x2 do esquematico oficial). USB via conector FPC 6 pinos "
            "e adaptador RP-Tiny-Adapter-B.")
    c = []
    for pid, name, hdr, i, col, row in pins:
        d = f"{name} ({hdr} pino {i})" + (f" - {EXTRA[name]}" if name in EXTRA else "")
        c.append(f'''    <connector id="connector{pid}" type="male" name="{escape(name)}">
      <description>{escape(d)}</description>
      <views>
        <breadboardView><p svgId="connector{pid}pin" layer="breadboard"/></breadboardView>
        <schematicView><p svgId="connector{pid}pin" layer="schematic" terminalId="connector{pid}terminal"/></schematicView>
        <pcbView>
          <p svgId="connector{pid}pin" layer="copper0"/>
          <p svgId="connector{pid}pin" layer="copper1"/>
        </pcbView>
      </views>
    </connector>''')
    bus = "\n".join(f'      <nodeMember connectorId="connector{g}"/>' for g in gnd)
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module moduleId="{MID}" referenceFile="{MID}.fzp" fritzingVersion="0.9.10">
  <version>1</version>
  <author>PicoDMG</author>
  <title>Waveshare Core2350B</title>
  <label>U</label>
  <date>2026-11-11</date>
  <url>https://www.waveshare.com/core2350b.htm</url>
  <tags>
    <tag>Waveshare</tag><tag>Core2350B</tag><tag>RP2350B</tag><tag>RP2350</tag>
    <tag>raspberry pi</tag><tag>cpu board</tag><tag>fritzing user</tag>
  </tags>
  <properties>
    <property name="family">Waveshare Core2350B</property>
    <property name="variant">variant 1</property>
    <property name="package">THT</property>
    <property name="pins">64</property>
    <property name="pin spacing">100mil</property>
    <property name="chip">RP2350B</property>
    <property name="editable pin labels">false</property>
    <property name="layer"/>
    <property name="part number">Core2350B</property>
  </properties>
  <description>{escape(desc)}</description>
  <views>
    <iconView><layers image="breadboard/{MID}_breadboard.svg"><layer layerId="icon"/></layers></iconView>
    <breadboardView><layers image="breadboard/{MID}_breadboard.svg"><layer layerId="breadboard"/></layers></breadboardView>
    <schematicView><layers image="schematic/{MID}_schematic.svg"><layer layerId="schematic"/></layers></schematicView>
    <pcbView>
      <layers image="pcb/{MID}_pcb.svg">
        <layer layerId="silkscreen"/>
        <layer layerId="copper0"/>
        <layer layerId="copper1"/>
      </layers>
    </pcbView>
  </views>
  <connectors>
{chr(10).join(c)}
  </connectors>
  <buses>
    <bus id="gnd">
{bus}
    </bus>
  </buses>
</module>
'''

files = {
    f"{MID}.fzp": fzp(),
    f"svg/breadboard/{MID}_breadboard.svg": breadboard(),
    f"svg/schematic/{MID}_schematic.svg": schematic(),
    f"svg/pcb/{MID}_pcb.svg": pcb(),
}
os.makedirs(OUT, exist_ok=True)
for rel, txt in files.items():
    p = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w", encoding="utf-8", newline="\n").write(txt)

# .fzpz: nomes planos com prefixo de vista (formato que o Fritzing exporta)
with zipfile.ZipFile(os.path.join(OUT, "Waveshare_Core2350B.fzpz"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"part.{MID}.fzp", files[f"{MID}.fzp"])
    for v in ("breadboard", "schematic", "pcb"):
        z.writestr(f"svg.{v}.{MID}_{v}.svg", files[f"svg/{v}/{MID}_{v}.svg"])

for p in sorted(pins, key=lambda p: (p[5], p[4])):
    pass
print("ok", len(pins), "pinos; GND:", gnd)
grid = [["" for _ in range(10)] for _ in range(10)]
for p in pins: grid[p[5]][p[4]] = p[1]
for row in grid: print(" ".join(f"{n:>8}" for n in row))
