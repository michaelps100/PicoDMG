"""Gera a peca Fritzing do adaptador USB-C femea para 4 furos PTH (V, D-, D+, G).

Placa verde de 12 x 13,8 mm, 4 furos a 2,54 mm numa fileira no alto e o
conector USB-C na borda de baixo. Medidas aproximadas, para ilustracao.
Unidades dos SVGs: 1/1000 pol.

Uso: python gen_usbc4p.py <pasta_saida>
"""
import os, sys, zipfile
from xml.sax.saxutils import escape

OUT = sys.argv[1]
MID = "usb-c-4pin-pth-adapter_1"
W, H = 472, 543            # 12 x 13,8 mm
PIN_Y = 100
PINS = [
    ("VBUS", "V",  "VBUS, 5 V do USB"),
    ("D-",   "D-", "USB D-"),
    ("D+",   "D+", "USB D+"),
    ("GND",  "G",  "terra"),
]
PIN_X = [W // 2 - 150 + 100 * i for i in range(4)]
SHELL = (61, 285, 350, 258)    # x, y, w, h do conector (sai pela borda de baixo)

HEAD = ('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" version="1.2" '
        'width="{w}in" height="{h}in" viewBox="0 0 {vw} {vh}">\n')
FONT = 'font-family="Droid Sans"'

def breadboard():
    sx, sy, sw, sh = SHELL
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="breadboard">\n',
         f'<rect x="0" y="0" width="{W}" height="{H-20}" rx="12" fill="#14a04a"/>\n',
         # carcaca metalica do USB-C
         f'<rect x="{sx}" y="{sy}" width="{sw}" height="{sh}" rx="18" fill="#d4d4d4" stroke="#8c8c8c" stroke-width="6"/>\n',
         f'<rect x="{sx+50}" y="{sy+sh-45}" width="{sw-100}" height="30" rx="14" fill="#5a5a5a"/>\n']
    for cx in (sx + 120, sx + sw - 120):
        s.append(f'<circle cx="{cx}" cy="{sy+90}" r="14" fill="#9a9a9a"/>\n')
    for pid, ((name, lbl, _), x) in enumerate(zip(PINS, PIN_X)):
        s.append(f'<rect id="connector{pid}pin" x="{x-38}" y="{PIN_Y-60}" width="76" height="98" rx="10" fill="#e6e6e6"/>\n')
        s.append(f'<circle cx="{x}" cy="{PIN_Y}" r="20" fill="#666666"/>\n')
        s.append(f'<text x="{x}" y="{PIN_Y+120}" {FONT} font-size="62" fill="#ffffff" text-anchor="middle">{escape(lbl)}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def schematic():
    SW, Y0 = 800, 300
    SH = Y0 + 3 * 100 + 200
    s = [HEAD.format(w=SW/1000, h=SH/1000, vw=SW, vh=SH), '<g id="schematic">\n',
         f'<rect x="100" y="150" width="{SW-200}" height="{SH-250}" fill="#ffffff" stroke="#000000" stroke-width="10"/>\n',
         f'<text x="{SW/2}" y="110" {FONT} font-size="60" fill="#000000" text-anchor="middle">USB-C</text>\n',
         '<rect x="500" y="330" width="120" height="240" rx="50" fill="none" stroke="#555555" stroke-width="8"/>\n']
    for pid, (name, _, _) in enumerate(PINS):
        y = Y0 + pid * 100
        s.append(f'<line id="connector{pid}pin" x1="0" y1="{y}" x2="100" y2="{y}" stroke="#555555" stroke-width="10"/>\n')
        s.append(f'<rect id="connector{pid}terminal" x="0" y="{y-5}" width="10" height="10" fill="none" stroke="none"/>\n')
        s.append(f'<text x="125" y="{y+17}" {FONT} font-size="50" fill="#555555">{escape(name)}</text>\n')
    s.append('</g>\n</svg>\n')
    return "".join(s)

def pcb():
    sx, sy, sw, sh = SHELL
    s = [HEAD.format(w=W/1000, h=H/1000, vw=W, vh=H), '<g id="silkscreen">\n',
         f'<rect x="4" y="4" width="{W-8}" height="{H-8}" rx="12" fill="none" stroke="#000000" stroke-width="8"/>\n',
         f'<rect x="{sx}" y="{sy}" width="{sw}" height="{sh-4}" fill="none" stroke="#000000" stroke-width="8"/>\n',
         '</g>\n<g id="copper1">\n<g id="copper0">\n']
    for pid, x in enumerate(PIN_X):
        if pid == 0:
            s.append(f'<rect id="connector{pid}pin" x="{x-30}" y="{PIN_Y-30}" width="60" height="60" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
        else:
            s.append(f'<circle id="connector{pid}pin" cx="{x}" cy="{PIN_Y}" r="30" fill="none" stroke="#f7bd13" stroke-width="20"/>\n')
    s.append('</g>\n</g>\n</svg>\n')
    return "".join(s)

def fzp():
    conns = []
    for pid, (name, lbl, d) in enumerate(PINS):
        conns.append(f'''    <connector id="connector{pid}" type="male" name="{escape(name)}">
      <description>{escape(f"{lbl} na placa - {d}")}</description>
      <views>
        <breadboardView><p svgId="connector{pid}pin" layer="breadboard"/></breadboardView>
        <schematicView><p svgId="connector{pid}pin" layer="schematic" terminalId="connector{pid}terminal"/></schematicView>
        <pcbView>
          <p svgId="connector{pid}pin" layer="copper0"/>
          <p svgId="connector{pid}pin" layer="copper1"/>
        </pcbView>
      </views>
    </connector>''')
    desc = ("Adaptador USB-C femea para 4 furos PTH a 2,54 mm (V, D-, D+, G). "
            "Placa de 12 x 13,8 mm. Dimensoes aproximadas, para ilustracao.")
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module moduleId="{MID}" referenceFile="{MID}.fzp" fritzingVersion="0.9.10">
  <version>1</version>
  <author>PicoDMG</author>
  <title>USB-C 4 pinos PTH adapter</title>
  <label>J</label>
  <date>2026-11-11</date>
  <tags>
    <tag>USB-C</tag><tag>USB Type-C</tag><tag>USB</tag><tag>breakout</tag>
    <tag>adapter</tag><tag>fritzing user</tag>
  </tags>
  <properties>
    <property name="family">USB-C adapter</property>
    <property name="variant">4 pinos PTH</property>
    <property name="package">THT</property>
    <property name="pins">4</property>
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
with zipfile.ZipFile(os.path.join(OUT, "USB-C_4pin_PTH_Adapter.fzpz"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"part.{MID}.fzp", files[f"{MID}.fzp"])
    for v in ("breadboard", "schematic", "pcb"):
        z.writestr(f"svg.{v}.{MID}_{v}.svg", files[f"svg/{v}/{MID}_{v}.svg"])
print("ok", len(PINS), "pinos")
