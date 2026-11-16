"""Gera a peca Fritzing do jack P2 (3,5 mm) SMD de 5 pinos, corpo 12 x 6 mm.

Medidas do desenho do fabricante (P.C.B LAYOUT, TOP VIEW), em mm, com x a partir
da face traseira do bocal (o bocal de 2 mm fica a esquerda) e y a partir da linha
central do corpo:
  pads de cima  4, 5, 6 em x = 1,0 / 5,2 / 11,3   (5,2 e 6,1 do desenho; 4 fica 1,6 antes do 3)
  pads de baixo 3, 2    em x = 2,6 / 9,6          (2,6 e 7 do desenho)
  pads de 1,5 mm de largura, 10 mm de ponta a ponta na vertical
  2 furos de posicionamento de 1,2 mm na linha central (pinos de 0,9 mm do corpo)
O desenho numera os pads de 2 a 6 (nao ha pino 1). A funcao de cada pad (FUNCS)
foi levantada na montagem, nao no desenho.
Unidades dos SVGs: 1/1000 pol.

Uso: python gen_jack35_smd5.py <pasta_saida>
"""
import os, sys, zipfile
from xml.sax.saxutils import escape

OUT = sys.argv[1]
MID = "jack-3.5mm-smd-5pin_1"
MM = 39.37
OX, OY = 2.6, 5.5            # deslocamento (mm) da origem do desenho para o SVG
W, H = round(15.2 * MM), round(11.0 * MM)
PAD_W, PAD_H = 1.5, 2.6
ROW_Y = 5.0 - PAD_H / 2      # centro dos pads: bordas externas a +-5 mm

PADS = [  # (numero, x_mm, y_mm)
    (2, 9.6,  ROW_Y),
    (3, 2.6,  ROW_Y),
    (4, 1.0, -ROW_Y),
    (5, 5.2, -ROW_Y),
    (6, 11.3, -ROW_Y),
]
HOLES = [(3.0, 0.0), (9.0, 0.0)]

# Funcao dos pads, levantada na montagem do PicoDMG:
#   2-6 fechados sem plugue e abertos com plugue -> 2 e a ponta, 6 o contato NF dela
#   3-4 abertos sem plugue e fechados com plugue -> 3 e a manga, 4 a chave NA dela
#   5 e o anel (no console vai em jumper com o 2, para mono nos dois lados)
FUNCS = {
    2: ("TIP",    "ponta (L). Sem plugue fica ligada ao pino 6; com plugue a ligacao 2-6 abre"),
    3: ("SLEEVE", "manga (retorno do fone). No PicoDMG vai ao OUT- do MAX98357A por um resistor em serie, nao ao GND"),
    4: ("SLV_SW", "chave normalmente aberta: fecha com o pino 3 quando o plugue e inserido"),
    5: ("RING",   "anel (R). No PicoDMG vai em jumper com o pino 2 (mono nos dois lados do fone)"),
    6: ("TIP_SW", "chave normalmente fechada com o pino 2: abre com o plugue inserido (corta o alto-falante)"),
}

def P(x, y):                 # mm do desenho -> unidades do SVG
    return round((x + OX) * MM), round((y + OY) * MM)

HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" version="1.2" '
        'width="{w}in" height="{h}in" viewBox="0 0 {vw} {vh}">\n')
FONT = 'font-family="Droid Sans"'

def rect_mm(x0, y0, x1, y1, extra):
    (ax, ay), (bx, by) = P(x0, y0), P(x1, y1)
    return f'<rect x="{ax}" y="{ay}" width="{bx-ax}" height="{by-ay}" {extra}/>\n'

def pad_rect(pid, x, y, extra):
    return rect_mm(x - PAD_W / 2, y - PAD_H / 2, x + PAD_W / 2, y + PAD_H / 2, f'id="connector{pid}pin" {extra}')

def breadboard():
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="breadboard">\n']
    for pid, (n, x, y) in enumerate(PADS):            # terminais metalicos por baixo do corpo
        s.append(pad_rect(pid, x, y, 'fill="#c8c8c8" stroke="#8a8a8a" stroke-width="6"'))
    s.append(rect_mm(0, -3, 12, 3, 'fill="#1c1c1c"'))                 # corpo 12 x 6
    s.append(rect_mm(-2, -2.5, 0, 2.5, 'fill="#2b2b2b"'))             # bocal 5 mm
    cx, cy = P(-1.0, 0)
    s.append(rect_mm(-2.4, -1.8, 0.4, 1.8, 'fill="#000000"'))         # boca do jack (3,6 mm)
    s.append(rect_mm(4.5, -2.2, 10.5, 2.2, 'fill="#9a9a9a" stroke="#6a6a6a" stroke-width="5"'))  # chapa
    tx, ty = P(7.5, 0.5)
    s.append(f'<text x="{tx}" y="{ty}" {FONT} font-size="50" fill="#333333" text-anchor="middle">3.5</text>\n')
    for pid, (n, x, y) in enumerate(PADS):
        lx, ly = P(x, y + (0.7 if y > 0 else 0.3))
        s.append(f'<text x="{lx}" y="{ly}" {FONT} font-size="42" fill="#222222" text-anchor="middle">{n}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def schematic():
    SW, Y0 = 1000, 300
    SH = Y0 + 4 * 100 + 200
    s = [HEAD.format(w=SW/1000, h=SH/1000, vw=SW, vh=SH), '<g id="schematic">\n',
         f'<rect x="100" y="150" width="{SW-200}" height="{SH-250}" fill="#ffffff" stroke="#000000" stroke-width="10"/>\n',
         f'<text x="{SW/2}" y="110" {FONT} font-size="60" fill="#000000" text-anchor="middle">Jack P2 3,5 mm</text>\n',
         # simbolo de plugue: corpo e ponta
         '<rect x="560" y="390" width="220" height="120" fill="none" stroke="#555555" stroke-width="8"/>\n',
         '<polygon points="780,390 850,450 780,510" fill="none" stroke="#555555" stroke-width="8"/>\n',
         f'<text x="670" y="580" {FONT} font-size="36" fill="#555555" text-anchor="middle">5 pinos SMD</text>\n']
    for k, n in enumerate([2, 6, 5, 3, 4]):     # ponta e sua chave, anel, manga e sua chave
        pid = [p[0] for p in PADS].index(n)
        y = Y0 + k * 100
        s.append(f'<line id="connector{pid}pin" x1="0" y1="{y}" x2="100" y2="{y}" stroke="#555555" stroke-width="10"/>\n')
        s.append(f'<rect id="connector{pid}terminal" x="0" y="{y-5}" width="10" height="10" fill="none" stroke="none"/>\n')
        s.append(f'<text x="125" y="{y+17}" {FONT} font-size="50" fill="#555555">{FUNCS[n][0]} ({n})</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def pcb():
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="silkscreen">\n',
         rect_mm(0, -3, 12, 3, 'fill="none" stroke="#000000" stroke-width="8"'),
         rect_mm(-2, -2.5, 0, 2.5, 'fill="none" stroke="#000000" stroke-width="8" stroke-dasharray="30,20"')]
    for x, y in HOLES:       # furos de posicionamento (sem cobre), marcados na serigrafia
        hx, hy = P(x, y)
        s.append(f'<circle cx="{hx}" cy="{hy}" r="{round(0.6*MM)}" fill="none" stroke="#000000" stroke-width="6"/>\n')
    s.append('</g>\n<g id="copper1">\n')
    for pid, (n, x, y) in enumerate(PADS):
        s.append(pad_rect(pid, x, y, 'fill="#f7bd13" stroke="none"'))
    s.append('</g>\n</svg>\n')
    return "".join(s)

def fzp():
    conns = []
    for pid, (n, x, y) in enumerate(PADS):
        conns.append(f'''    <connector id="connector{pid}" type="pad" name="{FUNCS[n][0]}">
      <description>{escape(f"pino {n} - {FUNCS[n][1]}")}</description>
      <views>
        <breadboardView><p svgId="connector{pid}pin" layer="breadboard"/></breadboardView>
        <schematicView><p svgId="connector{pid}pin" layer="schematic" terminalId="connector{pid}terminal"/></schematicView>
        <pcbView><p svgId="connector{pid}pin" layer="copper1"/></pcbView>
      </views>
    </connector>''')
    desc = ("Jack P2 (3,5 mm) estereo SMD de 5 pinos, corpo 12 x 6 x 4,4 mm com bocal de 5 mm, "
            "pads numerados de 2 a 6 conforme o desenho do fabricante, 2 pinos de posicionamento "
            "de 0,9 mm. 2 = ponta (com chave NF no 6), 5 = anel, 3 = manga (com chave NA no 4).")
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module moduleId="{MID}" referenceFile="{MID}.fzp" fritzingVersion="0.9.10">
  <version>1</version>
  <author>PicoDMG</author>
  <title>Jack P2 3.5mm SMD 5 pinos</title>
  <label>J</label>
  <date>2026-11-11</date>
  <tags>
    <tag>jack</tag><tag>3.5mm</tag><tag>P2</tag><tag>audio</tag><tag>headphone</tag>
    <tag>SMD</tag><tag>fritzing user</tag>
  </tags>
  <properties>
    <property name="family">audio jack 3.5mm</property>
    <property name="variant">SMD 5 pinos</property>
    <property name="package">SMD</property>
    <property name="pins">5</property>
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
with zipfile.ZipFile(os.path.join(OUT, "Jack_3.5mm_SMD_5pin.fzpz"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"part.{MID}.fzp", files[f"{MID}.fzp"])
    for v in ("breadboard", "schematic", "pcb"):
        z.writestr(f"svg.{v}.{MID}_{v}.svg", files[f"svg/{v}/{MID}_{v}.svg"])
print("ok", len(PADS), "pads")
