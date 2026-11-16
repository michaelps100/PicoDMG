"""Gera a peca Fritzing do modulo carregador TP4056 USB-C com protecao (DW01 + 8205A).

Placa preta de 15 x 20 mm (retrato), USB-C na borda de baixo. Pads:
  borda de cima, da esquerda para a direita: OUT+, B+, B-, OUT-
  ao lado do USB-C: IN+ (esquerda), IN- (direita)
Ligacoes internas do modulo (barramentos da peca):
  B+ = OUT+ (a celula vai direto para a saida)
  IN- = OUT- (GND da placa; o MOSFET de protecao fica entre B- e OUT-)
Posicoes aproximadas, para ilustracao. Unidades dos SVGs: 1/1000 pol.

Uso: python gen_tp4056.py <pasta_saida>
"""
import os, sys, zipfile
from xml.sax.saxutils import escape

OUT = sys.argv[1]
MID = "tp4056-usb-c-protected-module_1"
W, H = 591, 787            # 15 x 20 mm
TOP_Y, IN_Y = 70, 500
PINS = [  # (nome, x, y, descricao)
    ("OUT+", 70,  TOP_Y, "saida + (ligada direto ao B+)"),
    ("B+",   230, TOP_Y, "positivo da celula de litio"),
    ("B-",   361, TOP_Y, "negativo da celula. So a celula: o MOSFET de protecao fica entre B- e OUT-"),
    ("OUT-", 521, TOP_Y, "saida - (GND da placa, o mesmo do IN-)"),
    ("IN+",  70,  IN_Y,  "entrada 5 V (mesmo VBUS do USB-C)"),
    ("IN-",  521, IN_Y,  "entrada GND (mesmo GND do USB-C e do OUT-)"),
]
BUSES = {"bplus": ["OUT+", "B+"], "gnd": ["OUT-", "IN-"]}
USB = (135, 560, 321, 227)     # x, y, w, h do USB-C (ate a borda de baixo)

HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" version="1.2" '
        'width="{w}in" height="{h}in" viewBox="0 0 {vw} {vh}">\n')
FONT = 'font-family="Droid Sans"'

def breadboard():
    ux, uy, uw, uh = USB
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="breadboard">\n',
         f'<rect x="0" y="0" width="{W}" height="{H-25}" rx="15" fill="#141414"/>\n',
         '<rect x="190" y="200" width="210" height="160" fill="#2a2a2a"/>\n',                                   # TP4056
         f'<text x="295" y="290" {FONT} font-size="34" fill="#777777" text-anchor="middle">TP4056</text>\n',
         '<rect x="85" y="185" width="70" height="40" fill="#c9a679"/>\n',
         '<rect x="85" y="255" width="55" height="28" fill="#e04040"/>\n',          # LED de carga
         '<rect x="85" y="305" width="55" height="28" fill="#3070e0"/>\n',          # LED de carga completa
         '<rect x="430" y="195" width="80" height="50" fill="#2a2a2a"/>\n',         # DW01
         '<rect x="430" y="300" width="80" height="45" fill="#c9a679"/>\n',
         '<rect x="210" y="400" width="100" height="45" fill="#c9a679"/>\n',
         f'<rect x="{ux}" y="{uy}" width="{uw}" height="{uh}" rx="25" fill="#d4d4d4" stroke="#8c8c8c" stroke-width="6"/>\n',
         f'<rect x="{ux+45}" y="{uy+uh-55}" width="{uw-90}" height="32" rx="15" fill="#5a5a5a"/>\n']
    for pid, (name, x, y, _) in enumerate(PINS):
        s.append(f'<rect id="connector{pid}pin" x="{x-42}" y="{y-42}" width="84" height="84" rx="8" fill="#d8b47a"/>\n')
        s.append(f'<circle cx="{x}" cy="{y}" r="20" fill="#f4f4f4"/>\n')
        if y == TOP_Y:
            ty, tx = y + 95, x
        else:
            ty, tx = y + 100, x
        s.append(f'<text x="{tx}" y="{ty}" {FONT} font-size="38" fill="#ffffff" text-anchor="middle">{escape(name)}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def schematic():
    left, right = ["IN+", "IN-"], ["OUT+", "OUT-", "B+", "B-"]
    SW, Y0 = 1000, 300
    SH = Y0 + 3 * 100 + 200
    ids = {p[0]: i for i, p in enumerate(PINS)}
    s = [HEAD.format(w=SW/1000, h=SH/1000, vw=SW, vh=SH), '<g id="schematic">\n',
         f'<rect x="100" y="150" width="{SW-200}" height="{SH-250}" fill="#ffffff" stroke="#000000" stroke-width="10"/>\n',
         f'<text x="{SW/2}" y="110" {FONT} font-size="60" fill="#000000" text-anchor="middle">TP4056 + DW01</text>\n',
         f'<text x="{SW/2}" y="{SH-130}" {FONT} font-size="36" fill="#555555" text-anchor="middle">carregador Li-ion USB-C</text>\n']
    def put(name, idx, side):
        pid, y = ids[name], Y0 + idx * 100
        x1, x2, tx, anc, term = (0, 100, 125, "start", 0) if side == "L" else (SW - 100, SW, SW - 125, "end", SW - 10)
        s.append(f'<line id="connector{pid}pin" x1="{x1}" y1="{y}" x2="{x2}" y2="{y}" stroke="#555555" stroke-width="10"/>\n')
        s.append(f'<rect id="connector{pid}terminal" x="{term}" y="{y-5}" width="10" height="10" fill="none" stroke="none"/>\n')
        s.append(f'<text x="{tx}" y="{y+17}" {FONT} font-size="50" fill="#555555" text-anchor="{anc}">{escape(name)}</text>\n')
    for k, n in enumerate(left): put(n, k, "L")
    for k, n in enumerate(right): put(n, k, "R")
    s.append('</g>\n</svg>\n')
    return "".join(s)

def pcb():
    ux, uy, uw, uh = USB
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="silkscreen">\n',
         f'<rect x="4" y="4" width="{W-8}" height="{H-8}" rx="15" fill="none" stroke="#000000" stroke-width="8"/>\n',
         f'<rect x="{ux}" y="{uy}" width="{uw}" height="{uh-4}" fill="none" stroke="#000000" stroke-width="8"/>\n',
         '</g>\n<g id="copper1">\n<g id="copper0">\n']
    for pid, (name, x, y, _) in enumerate(PINS):
        s.append(f'<rect id="connector{pid}pin" x="{x-32}" y="{y-32}" width="64" height="64" fill="none" stroke="#f7bd13" stroke-width="22"/>\n')
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
    ids = {p[0]: i for i, p in enumerate(PINS)}
    buses = "\n".join(
        f'    <bus id="{b}">\n' + "\n".join(f'      <nodeMember connectorId="connector{ids[n]}"/>' for n in members) + "\n    </bus>"
        for b, members in BUSES.items())
    desc = ("Modulo carregador de bateria de litio TP4056 (1 A) com protecao DW01 + 8205A e entrada USB-C. "
            "Placa de 15 x 20 mm. B+ e OUT+ sao o mesmo no; IN- e OUT- sao o GND da placa; "
            "B- vai so na celula. Posicoes aproximadas, para ilustracao.")
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module moduleId="{MID}" referenceFile="{MID}.fzp" fritzingVersion="0.9.10">
  <version>1</version>
  <author>PicoDMG</author>
  <title>TP4056 USB-C com protecao</title>
  <label>U</label>
  <date>2026-11-11</date>
  <tags>
    <tag>TP4056</tag><tag>DW01</tag><tag>charger</tag><tag>lithium</tag><tag>Li-ion</tag>
    <tag>battery</tag><tag>USB-C</tag><tag>fritzing user</tag>
  </tags>
  <properties>
    <property name="family">TP4056 charger module</property>
    <property name="variant">USB-C com protecao</property>
    <property name="package">THT</property>
    <property name="pins">6</property>
    <property name="chip">TP4056</property>
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
  <buses>
{buses}
  </buses>
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
with zipfile.ZipFile(os.path.join(OUT, "TP4056_USB-C_Protected.fzpz"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"part.{MID}.fzp", files[f"{MID}.fzp"])
    for v in ("breadboard", "schematic", "pcb"):
        z.writestr(f"svg.{v}.{MID}_{v}.svg", files[f"svg/{v}/{MID}_{v}.svg"])
print("ok", len(PINS), "pinos")
