"""Gera a peca Fritzing do modulo micro SD SPI (placa azul, 6 pinos, sem regulador).

Placa de 18,5 x 17,5 mm, 6 furos a 2,54 mm numa coluna junto a borda direita
(3V3, CS, MOSI, CLK, MISO, GND, de cima para baixo), slot do cartao na borda
esquerda. Desenho do lado dos rotulos (verso do slot). Medidas aproximadas, para
ilustracao. Unidades dos SVGs: 1/1000 pol.

Uso: python gen_microsd.py <pasta_saida>
"""
import os, sys, zipfile
from xml.sax.saxutils import escape

OUT = sys.argv[1]
MID = "microsd-spi-module-3v3_1"
W, H = 728, 689            # 18,5 x 17,5 mm
PX, PY0 = 670, 95          # coluna de pinos: borda direita, centrada na altura

PINS = [
    ("3V3",  "alimentacao 3,3 V (o modulo nao tem regulador: nao ligar em 5 V)"),
    ("CS",   "chip select do SPI (ativo em nivel baixo)"),
    ("MOSI", "dados do host para o cartao (DI do cartao)"),
    ("CLK",  "clock do SPI (SCK)"),
    ("MISO", "dados do cartao para o host (DO do cartao)"),
    ("GND",  "terra"),
]
PIN_XY = [(PX, PY0 + 100 * i) for i in range(len(PINS))]
HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" version="1.2" '
        'width="{w}in" height="{h}in" viewBox="0 0 {vw} {vh}">\n')
FONT = 'font-family="Droid Sans"'
HOLES = [(430, 70, 42), (430, 619, 42), (240, 175, 30), (240, 515, 30)]   # x, y, r

def breadboard():
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="breadboard">\n',
         f'<rect x="0" y="0" width="{W}" height="{H}" rx="20" fill="#1f2f80"/>\n',
         # slot do micro SD na borda esquerda
         '<rect x="0" y="60" width="95" height="570" fill="#2a2a2a"/>\n',
         '<rect x="95" y="80" width="20" height="530" fill="#b8b8b8"/>\n']
    for x, y, r in HOLES:
        s.append(f'<circle cx="{x}" cy="{y}" r="{r+12}" fill="#c8c8c8"/><circle cx="{x}" cy="{y}" r="{r}" fill="#111111"/>\n')
    s.append(f'<text x="420" y="520" {FONT} font-size="34" fill="#ffffff" transform="rotate(-90 420 520)">V474</text>\n')
    for pid, ((name, _), (x, y)) in enumerate(zip(PINS, PIN_XY)):
        if pid == 0:
            s.append(f'<rect id="connector{pid}pin" x="{x-36}" y="{y-36}" width="72" height="72" fill="#e8e8e8"/>\n')
        else:
            s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{y}" r="36" fill="#e8e8e8"/>\n')
        s.append(f'<circle cx="{x}" cy="{y}" r="18" fill="#7a7a7a"/>\n')
        s.append(f'<text x="{x-60}" y="{y+17}" {FONT} font-size="48" fill="#ffffff" text-anchor="end">{name}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def schematic():
    SW, Y0 = 900, 300
    SH = Y0 + (len(PINS) - 1) * 100 + 200
    s = [HEAD.format(w=SW/1000, h=SH/1000, vw=SW, vh=SH), '<g id="schematic">\n',
         f'<rect x="100" y="150" width="{SW-200}" height="{SH-250}" fill="#ffffff" stroke="#000000" stroke-width="10"/>\n',
         f'<text x="{SW/2+50}" y="110" {FONT} font-size="60" fill="#000000" text-anchor="middle">micro SD (SPI)</text>\n',
         f'<rect x="620" y="330" width="130" height="170" rx="10" fill="none" stroke="#555555" stroke-width="8"/>\n',
         f'<text x="685" y="440" {FONT} font-size="40" fill="#555555" text-anchor="middle">SD</text>\n']
    for pid, (name, _) in enumerate(PINS):
        y = Y0 + pid * 100
        s.append(f'<line id="connector{pid}pin" x1="0" y1="{y}" x2="100" y2="{y}" stroke="#555555" stroke-width="10"/>\n')
        s.append(f'<rect id="connector{pid}terminal" x="0" y="{y-5}" width="10" height="10" fill="none" stroke="none"/>\n')
        s.append(f'<text x="125" y="{y+17}" {FONT} font-size="50" fill="#555555">{name}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def pcb():
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="silkscreen">\n',
         f'<rect x="4" y="4" width="{W-8}" height="{H-8}" rx="20" fill="none" stroke="#000000" stroke-width="8"/>\n',
         '<rect x="4" y="60" width="111" height="570" fill="none" stroke="#000000" stroke-width="8"/>\n']
    for x, y, r in HOLES:
        s.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="none" stroke="#000000" stroke-width="8"/>\n')
    s.append('</g>\n<g id="copper1">\n<g id="copper0">\n')
    for pid, (x, y) in enumerate(PIN_XY):
        if pid == 0:     # 3V3 = pino 1, quadrado como na placa
            s.append(f'<rect id="connector{pid}pin" x="{x-30}" y="{y-30}" width="60" height="60" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
        else:
            s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{y}" r="30" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
    s.append('</g>\n</g>\n</svg>\n')
    return "".join(s)

def fzp():
    conns = []
    for pid, (name, d) in enumerate(PINS):
        conns.append(f'''    <connector id="connector{pid}" type="male" name="{escape(name)}">
      <description>{escape(name + " - " + d)}</description>
      <views>
        <breadboardView><p svgId="connector{pid}pin" layer="breadboard"/></breadboardView>
        <schematicView><p svgId="connector{pid}pin" layer="schematic" terminalId="connector{pid}terminal"/></schematicView>
        <pcbView>
          <p svgId="connector{pid}pin" layer="copper0"/>
          <p svgId="connector{pid}pin" layer="copper1"/>
        </pcbView>
      </views>
    </connector>''')
    desc = ("Modulo leitor de cartao micro SD por SPI, 3,3 V sem regulador. Placa azul de "
            "18,5 x 17,5 mm com 6 furos a 2,54 mm (3V3, CS, MOSI, CLK, MISO, GND). "
            "Dimensoes aproximadas, para ilustracao.")
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module moduleId="{MID}" referenceFile="{MID}.fzp" fritzingVersion="0.9.10">
  <version>1</version>
  <author>PicoDMG</author>
  <title>micro SD module (SPI, 3V3)</title>
  <label>SD</label>
  <date>2026-11-11</date>
  <tags>
    <tag>micro SD</tag><tag>microSD</tag><tag>SD card</tag><tag>TF card</tag>
    <tag>SPI</tag><tag>fritzing user</tag>
  </tags>
  <properties>
    <property name="family">micro SD module</property>
    <property name="variant">SPI 3V3 6 pinos</property>
    <property name="package">THT</property>
    <property name="pins">6</property>
    <property name="pin spacing">100mil</property>
    <property name="editable pin labels">false</property>
    <property name="layer"/>
    <property name="part number"/>
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
{chr(10).join(conns)}
  </connectors>
</module>
'''

files = {
    f"{MID}.fzp": fzp(),
    f"svg/breadboard/{MID}_breadboard.svg": breadboard(),
    f"svg/schematic/{MID}_schematic.svg": schematic(),
    f"svg/pcb/{MID}_pcb.svg": pcb(),
}
for rel, txt in files.items():
    p = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w", encoding="utf-8", newline="\n").write(txt)
with zipfile.ZipFile(os.path.join(OUT, "microSD_SPI_Module.fzpz"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"part.{MID}.fzp", files[f"{MID}.fzp"])
    for v in ("breadboard", "schematic", "pcb"):
        z.writestr(f"svg.{v}.{MID}_{v}.svg", files[f"svg/{v}/{MID}_{v}.svg"])
print("ok", len(PINS), "pinos")
