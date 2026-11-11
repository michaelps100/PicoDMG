# Licenças

Este repositório reúne projetos de licenças diferentes. Cada pasta mantém a licença do
projeto de onde veio; o arquivo de licença original continua dentro dela.

| Pasta | Licença | Arquivo / origem |
| :--- | :--- | :--- |
| `pico-bootLoader` | GPL-3.0 | `pico-bootLoader/LICENSE` |
| `PicoMD` | GPL-3.0 | `PicoMD/LICENSE` |
| `PicoNES` | GPL-3.0 | `PicoNES/LICENSE` |
| `PicoPCE` | GPL-3.0 | `PicoPCE/LICENSE` |
| `PicoSMS` | GPL-3.0 | `PicoSMS/LICENSE` |
| `PicoNGP` | GPL-2.0 (núcleo NeoPop) | cabeçalhos em `PicoNGP/src/neopop/` |
| `PicoA26` | MIT | `PicoA26/LICENSE` |
| `Pico8` | MIT | `Pico8/LICENSE.md` |
| `PicoGBC` | MIT (Peanut-GB) + licenças de `src/ext/*` | `PicoGBC/src/ext/*/LICENSE` |
| `PicoLINX` | Licença do Handy (K. Wilkins) | `PicoLINX/src/lynx/license.txt` |
| `PicoWS` | **a confirmar** (núcleo Oswan; upstream sem LICENSE) | [xrip/pico-wonderswan](https://github.com/xrip/pico-wonderswan) |
| `PicoSNES` | **Snes9x: somente uso não comercial** | `PicoSNES/src/copyright.h` |
| `*/pico_shared`, `*/pico_lib`, `*/tusb_xinput` | as do upstream PicoPlus-devel | [PicoPlus-devel](https://github.com/PicoPlus-devel) |
| `PicoPCE/external/libchdr` | BSD-3-Clause | `PicoPCE/external/libchdr/LICENSE.txt` |
| `shared_menu` | GPL-3.0 | este repositório |

## Notas

- **`shared_menu` é linkada em todos os emuladores**, inclusive nos GPL, então é
  distribuída como GPL-3.0.
- **PicoSNES**: a licença do Snes9x permite copiar, modificar e distribuir apenas para
  fins **não comerciais**. Vender o console com o PicoSNES gravado exige permissão dos
  detentores do Snes9x.
- **PicoWS**: o upstream não declara licença. Até confirmar, considere a pasta "todos os
  direitos reservados aos autores originais".
- As cópias de `pico_shared/`, `pico_lib/` e `tusb_xinput/` estavam como submódulos do
  PicoPlus-devel e foram incorporadas como arquivos normais, porque cada emulador tem
  alterações locais nelas.
