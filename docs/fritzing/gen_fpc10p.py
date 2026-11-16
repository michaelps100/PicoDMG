"""Gera a peca Fritzing do adaptador FPC/FFC 10 pinos 0,5 mm para header 2,54 mm.

Placa verde de 20 x 26 mm (retrato), 10 furos em 2 x 5 a 2,54 mm no alto
(pares em cima: 10 8 6 4 2; impares embaixo: 9 7 5 3 1, da esquerda para a
direita), dois furos de fixacao e o conector FPC na borda de baixo. O pino N do
header corresponde ao contato N do FPC. Medidas aproximadas, para ilustracao.
Unidades dos SVGs: 1/1000 pol.

Uso: python gen_fpc10p.py <pasta_saida>
"""
import os, sys, zipfile
from xml.sax.saxutils import escape

OUT = sys.argv[1]
MID = "fpc-10p-0.5mm-adapter_1"
W, H = 787, 1024           # 20 x 26 mm
CX = W // 2
Y_EVEN, Y_ODD = 230, 330

def pin_xy(n):             # n = 1..10
    col = 4 - (n - 1) // 2                  # pino 1/2 na coluna da direita
    return CX + (col - 2) * 100, (Y_ODD if n % 2 else Y_EVEN)

PINS = list(range(1, 11))
HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" version="1.2" '
        'width="{w}in" height="{h}in" viewBox="0 0 {vw} {vh}">\n')
FONT = 'font-family="Droid Sans"'
HOLES = [(118, 640, 60), (669, 640, 60)]
FPC = (190, 840, 407, 150)   # x, y, w, h do conector

def breadboard():
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="breadboard">\n',
         f'<rect x="0" y="0" width="{W}" height="{H}" rx="25" fill="#0f7a34"/>\n']
    # trilhas do conector ate os furos
    fx, fy, fw, fh = FPC
    for n in PINS:
        x, y = pin_xy(n)
        cx = fx + 40 + (10 - n) * (fw - 80) / 9
        s.append(f'<polyline points="{x},{y} {x},{y+90} {cx:.0f},{fy-60} {cx:.0f},{fy+20}" '
                 f'fill="none" stroke="#1d9a48" stroke-width="10"/>\n')
    for x, y, r in HOLES:
        s.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="#ffffff"/>\n')
    s.append(f'<text x="{CX}" y="510" {FONT} font-size="70" fill="#ffffff" text-anchor="middle">FPC-10P 0.5MM</text>\n')
    # conector FPC (corpo, contatos e trava)
    s.append(f'<rect x="{fx}" y="{fy}" width="{fw}" height="{fh}" fill="#e8e8e8" stroke="#9a9a9a" stroke-width="6"/>\n')
    for k in range(10):
        x = fx + 40 + k * (fw - 80) / 9
        s.append(f'<rect x="{x-6:.0f}" y="{fy+20}" width="12" height="50" fill="#c9a679"/>\n')
    s.append(f'<rect x="{fx+20}" y="{fy+90}" width="{fw-40}" height="50" fill="#2a2a2a"/>\n')
    for n in PINS:
        x, y = pin_xy(n)
        if n == 1:
            s.append(f'<rect id="connector{n-1}pin" x="{x-36}" y="{y-36}" width="72" height="72" fill="#e0e0e0"/>\n')
        else:
            s.append(f'<circle id="connector{n-1}pin" cx="{x}" cy="{y}" r="36" fill="#e0e0e0"/>\n')
        s.append(f'<circle cx="{x}" cy="{y}" r="18" fill="#555555"/>\n')
        ty = y - 55 if n % 2 == 0 else y + 85
        s.append(f'<text x="{x}" y="{ty}" {FONT} font-size="40" fill="#ffffff" text-anchor="middle">{n}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def schematic():
    SW, Y0 = 800, 300
    SH = Y0 + 9 * 100 + 200
    s = [HEAD.format(w=SW/1000, h=SH/1000, vw=SW, vh=SH), '<g id="schematic">\n',
         f'<rect x="100" y="150" width="{SW-200}" height="{SH-250}" fill="#ffffff" stroke="#000000" stroke-width="10"/>\n',
         f'<text x="{SW/2}" y="110" {FONT} font-size="60" fill="#000000" text-anchor="middle">FPC-10P 0.5mm</text>\n']
    for n in PINS:
        y = Y0 + (n - 1) * 100
        s.append(f'<line id="connector{n-1}pin" x1="0" y1="{y}" x2="100" y2="{y}" stroke="#555555" stroke-width="10"/>\n')
        s.append(f'<rect id="connector{n-1}terminal" x="0" y="{y-5}" width="10" height="10" fill="none" stroke="none"/>\n')
        s.append(f'<text x="125" y="{y+17}" {FONT} font-size="50" fill="#555555">{n}</text>\n')
    # simbolo do conector: 10 contatos lado a lado
    s.append(f'<rect x="460" y="{Y0}" width="160" height="900" fill="none" stroke="#555555" stroke-width="8"/>\n')
    for n in PINS:
        y = Y0 + (n - 1) * 100
        s.append(f'<line x1="300" y1="{y}" x2="500" y2="{y}" stroke="#aaaaaa" stroke-width="6"/>\n')
    s.append(f'<text x="540" y="{Y0+470}" {FONT} font-size="40" fill="#555555" text-anchor="middle" '
             f'transform="rotate(-90 540 {Y0+470})">FPC</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def pcb():
    fx, fy, fw, fh = FPC
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="silkscreen">\n',
         f'<rect x="4" y="4" width="{W-8}" height="{H-8}" rx="25" fill="none" stroke="#000000" stroke-width="8"/>\n',
         f'<rect x="{fx}" y="{fy}" width="{fw}" height="{fh}" fill="none" stroke="#000000" stroke-width="8"/>\n']
    for x, y, r in HOLES:
        s.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="none" stroke="#000000" stroke-width="8"/>\n')
    s.append('</g>\n<g id="copper1">\n<g id="copper0">\n')
    for n in PINS:
        x, y = pin_xy(n)
        if n == 1:
            s.append(f'<rect id="connector{n-1}pin" x="{x-30}" y="{y-30}" width="60" height="60" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
        else:
            s.append(f'<circle id="connector{n-1}pin" cx="{x}" cy="{y}" r="30" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
    s.append('</g>\n</g>\n</svg>\n')
    return "".join(s)

def fzp():
    conns = []
    for n in PINS:
        pid = n - 1
        conns.append(f'''    <connector id="connector{pid}" type="male" name="{n}">
      <description>pino {n} do header = contato {n} do FPC</description>
      <views>
        <breadboardView><p svgId="connector{pid}pin" layer="breadboard"/></breadboardView>
        <schematicView><p svgId="connector{pid}pin" layer="schematic" terminalId="connector{pid}terminal"/></schematicView>
        <pcbView>
          <p svgId="connector{pid}pin" layer="copper0"/>
          <p svgId="connector{pid}pin" layer="copper1"/>
        </pcbView>
      </views>
    </connector>''')
    desc = ("Adaptador de cabo FPC/FFC de 10 vias, passo 0,5 mm, para header 2 x 5 de 2,54 mm. "
            "Placa de 20 x 26 mm. O pino N do header e o contato N do FPC. "
            "Dimensoes aproximadas, para ilustracao.")
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module moduleId="{MID}" referenceFile="{MID}.fzp" fritzingVersion="0.9.10">
  <version>1</version>
  <author>PicoDMG</author>
  <title>FPC-10P 0.5mm adapter</title>
  <label>J</label>
  <date>2026-11-11</date>
  <tags>
    <tag>FPC</tag><tag>FFC</tag><tag>10 pin</tag><tag>0.5mm</tag><tag>adapter</tag>
    <tag>breakout</tag><tag>fritzing user</tag>
  </tags>
  <properties>
    <property name="family">FPC adapter</property>
    <property name="variant">10P 0.5mm</property>
    <property name="package">THT</property>
    <property name="pins">10</property>
    <property name="pin spacing">100mil</property>
    <property name="layout">Double Row</property>
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
with zipfile.ZipFile(os.path.join(OUT, "FPC_10P_0.5mm_Adapter.fzpz"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"part.{MID}.fzp", files[f"{MID}.fzp"])
    for v in ("breadboard", "schematic", "pcb"):
        z.writestr(f"svg.{v}.{MID}_{v}.svg", files[f"svg/{v}/{MID}_{v}.svg"])
print("ok", len(PINS), "pinos")
