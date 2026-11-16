"""Gera a peca Fritzing do modulo amplificador I2S MAX98357A (placa roxa, 7 pinos).

Dimensoes aproximadas da foto do modulo: 17,7 x 19,1 mm, header de 7 pinos a
2,54 mm, furos de fixacao a 12,6 mm. Os dois pads do borne do alto-falante (- e +)
foram postos a 0,2" um do outro para ficarem na grade; e uma ilustracao, nao um
footprint medido. Unidades dos SVGs: 1/1000 pol.

Uso: python gen_max98357a.py <pasta_saida>
"""
import os, sys, zipfile
from xml.sax.saxutils import escape

OUT = sys.argv[1]
MID = "max98357a-i2s-amp_1"
W, H = 700, 750            # 17,8 x 19,1 mm
PIN_Y = 630

# (nome, x, y, descricao)
PINS = [
    ("LRC",  50,  PIN_Y, "LRCLK / word select do I2S"),
    ("BCLK", 150, PIN_Y, "bit clock do I2S"),
    ("DIN",  250, PIN_Y, "dados do I2S"),
    ("GAIN", 350, PIN_Y, "ganho: aberto = 9 dB, GND = 12 dB, Vin = 6 dB (100k p/ GND = 15 dB, 100k p/ Vin = 3 dB)"),
    ("SD",   450, PIN_Y, "shutdown / canal: GND = desligado; aberto = (E+D)/2 com o pull-up da placa"),
    ("GND",  550, PIN_Y, "terra"),
    ("Vin",  650, PIN_Y, "alimentacao 2,5 a 5,5 V"),
    ("SPK-", 250, 200,   "saida do alto-falante (-). Saida em ponte: nao ligar ao GND"),
    ("SPK+", 450, 200,   "saida do alto-falante (+). Saida em ponte: nao ligar ao GND"),
]
HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" version="1.2" '
        'width="{w}in" height="{h}in" viewBox="0 0 {vw} {vh}">\n')
FONT = 'font-family="Droid Sans"'

def outline(fill, stroke="none", sw=0):
    return (f'<rect x="{sw/2}" y="{sw/2}" width="{W-sw}" height="{H-sw}" rx="70" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>\n')

def breadboard():
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="breadboard">\n', outline("#7b2d8e")]
    for hx in (102, 598):                                       # furos de fixacao
        s.append(f'<circle cx="{hx}" cy="95" r="62" fill="#d8b47a"/><circle cx="{hx}" cy="95" r="45" fill="#ffffff"/>\n')
    # borne do alto-falante
    s.append('<rect x="170" y="60" width="360" height="270" fill="none" stroke="#ffffff" stroke-width="8"/>\n')
    s.append(f'<text x="250" y="305" {FONT} font-size="60" fill="#ffffff" text-anchor="middle">-</text>\n')
    s.append(f'<text x="450" y="305" {FONT} font-size="60" fill="#ffffff" text-anchor="middle">+</text>\n')
    for cx in (120, 270, 430, 580):                              # capacitores / ferrites
        s.append(f'<rect x="{cx-35}" y="360" width="70" height="40" fill="#c9a679" stroke="#ddd" stroke-width="4"/>\n')
    s.append('<rect x="305" y="430" width="90" height="90" fill="#222222"/>\n')   # MAX98357A (TQFN)
    s.append('<rect x="470" y="440" width="50" height="70" fill="#c9a679"/>\n')
    s.append(f'<text x="40" y="455" {FONT} font-size="40" fill="#ffffff">MAX</text>\n')
    s.append(f'<text x="40" y="500" {FONT} font-size="40" fill="#ffffff">98357A</text>\n')
    s.append(f'<text x="40" y="545" {FONT} font-size="34" fill="#ffffff">I2S Amp</text>\n')
    for pid, (name, x, y, _) in enumerate(PINS):
        r, hole = (36, 18) if y == PIN_Y else (45, 25)
        s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{y}" r="{r}" fill="#d8b47a"/>\n')
        s.append(f'<circle cx="{x}" cy="{y}" r="{hole}" fill="#f4f4f4"/>\n')
        if y == PIN_Y:       # rotulos alternados como na serigrafia
            ty = PIN_Y - 55 if pid % 2 == 0 else PIN_Y + 80
            s.append(f'<text x="{x}" y="{ty}" {FONT} font-size="32" fill="#ffffff" text-anchor="middle">{name}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def schematic():
    left = ["LRC", "BCLK", "DIN", "GAIN", "SD"]
    right = ["Vin", "GND", "SPK+", "SPK-"]
    SW, Y0 = 1000, 300
    SH = Y0 + 4 * 100 + 200
    ids = {p[0]: i for i, p in enumerate(PINS)}
    s = [HEAD.format(w=SW/1000, h=SH/1000, vw=SW, vh=SH), '<g id="schematic">\n',
         f'<rect x="100" y="150" width="{SW-200}" height="{SH-250}" fill="#ffffff" stroke="#000000" stroke-width="10"/>\n',
         f'<text x="{SW/2}" y="110" {FONT} font-size="60" fill="#000000" text-anchor="middle">MAX98357A</text>\n',
         f'<text x="{SW/2}" y="{SH-120}" {FONT} font-size="40" fill="#555555" text-anchor="middle">I2S Amp</text>\n']
    def put(name, idx, side):
        pid, y = ids[name], Y0 + idx * 100
        x1, x2, tx, anc, term = (0, 100, 125, "start", 0) if side == "L" else (SW - 100, SW, SW - 125, "end", SW - 10)
        s.append(f'<line id="connector{pid}pin" x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" stroke="#555555" stroke-width="10"/>\n')
        s.append(f'<rect id="connector{pid}terminal" x="{term}" y="{y-5}" width="10" height="10" fill="none" stroke="none"/>\n')
        s.append(f'<text x="{tx}" y="{y+17}" {FONT} font-size="50" fill="#555555" text-anchor="{anc}">{name}</text>\n')
    for k, n in enumerate(left): put(n, k, "L")
    for k, n in enumerate(right): put(n, k, "R")
    s.append('</g>\n</svg>\n')
    return "".join(s)

def pcb():
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="silkscreen">\n', outline("none", "#000000", 8),
         '<rect x="170" y="60" width="360" height="270" fill="none" stroke="#000000" stroke-width="8"/>\n',
         '<circle cx="102" cy="95" r="62" fill="none" stroke="#000000" stroke-width="8"/>\n',
         '<circle cx="598" cy="95" r="62" fill="none" stroke="#000000" stroke-width="8"/>\n',
         '</g>\n<g id="copper1">\n<g id="copper0">\n']
    for pid, (name, x, y, _) in enumerate(PINS):
        if y != PIN_Y:
            s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{y}" r="38" fill="none" stroke="#f7bd13" stroke-width="24"/>\n')
        elif pid == 0:       # pino 1 quadrado
            s.append(f'<rect id="connector{pid}pin" x="{x-30}" y="{y-30}" width="60" height="60" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
        else:
            s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{y}" r="30" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
    s.append('</g>\n</g>\n</svg>\n')
    return "".join(s)

def fzp():
    conns = []
    for pid, (name, x, y, d) in enumerate(PINS):
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
    desc = ("Modulo amplificador classe D I2S MAX98357A (3 W em 4 ohm a 5 V), placa roxa "
            "de 17,7 x 19,1 mm com header de 7 pinos a 2,54 mm e borne para o alto-falante. "
            "Dimensoes aproximadas, para ilustracao.")
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module moduleId="{MID}" referenceFile="{MID}.fzp" fritzingVersion="0.9.10">
  <version>1</version>
  <author>PicoDMG</author>
  <title>MAX98357A I2S Amp</title>
  <label>AMP</label>
  <date>2026-11-11</date>
  <tags>
    <tag>MAX98357A</tag><tag>MAX98357</tag><tag>I2S</tag><tag>amplifier</tag>
    <tag>audio</tag><tag>fritzing user</tag>
  </tags>
  <properties>
    <property name="family">MAX98357A I2S Amp</property>
    <property name="variant">variant 1</property>
    <property name="package">THT</property>
    <property name="pins">9</property>
    <property name="pin spacing">100mil</property>
    <property name="chip">MAX98357A</property>
    <property name="editable pin labels">false</property>
    <property name="layer"/>
    <property name="part number">MAX98357A</property>
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
with zipfile.ZipFile(os.path.join(OUT, "MAX98357A_I2S_Amp.fzpz"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"part.{MID}.fzp", files[f"{MID}.fzp"])
    for v in ("breadboard", "schematic", "pcb"):
        z.writestr(f"svg.{v}.{MID}_{v}.svg", files[f"svg/{v}/{MID}_{v}.svg"])
print("ok", len(PINS), "pinos")
