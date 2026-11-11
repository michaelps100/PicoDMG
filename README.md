# PicoDMG

Console portátil com emuladores para o **Raspberry Pi RP2350**. Um bootloader no SD
lista os emuladores, grava o escolhido na flash e entrega o controle a ele. Todos
compartilham o mesmo seletor de ROMs, o menu in-game, os combos, o controle de volume,
a chave liga/desliga com savestate e o indicador de bateria (`shared_menu/`).

## Placas

| Nome no código | Placa | Display | Script de build |
| :--- | :--- | :--- | :--- |
| **HW15** | Waveshare RP2350 Plus 16 MB (carcaça Game Boy DMG) | ILI9341 320x240 | `build_for_bootloader.ps1` |
| **HW16** | Waveshare Core2350B (PSRAM) | ST7789 320x240 | `build_core2350b.ps1` |

Áudio I2S por MAX98357A. Controles em GP2..GP9.

## Emuladores

| Pasta | Sistema | Extensões | Upstream |
| :--- | :--- | :--- | :--- |
| `PicoA26` | Atari 2600 | `.a26` `.bin` | [xrip/pico-atari2600](https://github.com/xrip/pico-atari2600) |
| `PicoGBC` | Game Boy / Color (gera PicoGBC e PicoDMG) | `.gb` `.gbc` | [dodger-one/PicoDMG](https://github.com/dodger-one/PicoDMG) |
| `PicoLINX` | Atari Lynx | `.lnx` `.lyx` `.o` | [xrip/pico-lynx](https://github.com/xrip/pico-lynx) |
| `PicoMD` | Mega Drive / Genesis | `.md` `.bin` `.gen` `.smd` | [PicoPlus-devel/pico-genesisPlus](https://github.com/PicoPlus-devel/pico-genesisPlus) |
| `PicoNES` | NES / Famicom Disk System | `.nes` `.fds` `.nsf` | [PicoPlus-devel/pico-infonesPlus](https://github.com/PicoPlus-devel/pico-infonesPlus) |
| `PicoNGP` | Neo Geo Pocket / Color | `.ngp` `.ngc` | [xrip/pico-neogeo-pocket](https://github.com/xrip/pico-neogeo-pocket) |
| `PicoPCE` | PC Engine / SuperGrafx (CD via CHD no HW16) | `.pce` `.sgx` `.cue` `.chd` | [PicoPlus-devel/pico-pcePlus](https://github.com/PicoPlus-devel/pico-pcePlus) |
| `PicoSMS` | Master System / Game Gear (gera PicoSMS e PicoGG) | `.sms` `.gg` `.sg` | [PicoPlus-devel/pico-smsplus](https://github.com/PicoPlus-devel/pico-smsplus) |
| `PicoSNES` | Super Nintendo | `.smc` `.sfc` `.fig` | [libretro/snes9x2002](https://github.com/libretro/snes9x2002) |
| `PicoWS` | WonderSwan / Color | `.ws` `.wsc` | [xrip/pico-wonderswan](https://github.com/xrip/pico-wonderswan) |
| `Pico8` | PICO-8 (yocto-8) | `.p8` `.png` | [yocto-8/yocto-8](https://github.com/yocto-8/yocto-8) |
| `pico-bootLoader` | Bootloader / lançador | | [PicoPlus-devel/pico-bootLoader](https://github.com/PicoPlus-devel/pico-bootLoader) |

Cada pasta foi importada com o histórico do upstream (`git log -- PicoNES`). Cada
emulador tem a sua própria cópia de `pico_shared/` com o driver de display modificado:
elas **não** são intercambiáveis.

## Compilar

Requisitos: a extensão Raspberry Pi Pico do VS Code (ou o mesmo conjunto instalado em
`%USERPROFILE%\.pico-sdk`): SDK 2.3.1, toolchain Arm 15.2, CMake 3.28, Ninja 1.12,
picotool 2.3.1.

Um emulador, uma placa:

```powershell
cd PicoNES
.\build_for_bootloader.ps1   # HW15 -> PicoNES.uf2
.\build_core2350b.ps1        # HW16 -> PicoNES_hw16.uf2
```

Tudo:

```powershell
.\tools\build_all.ps1 -Board HW16   # ou HW15, ou All
```

O bootloader: `pico-bootLoader\build_ili9341.ps1` (HW15) e `build_core2350b.ps1` (HW16).

## Cartão SD

```
0:/boot.txt          configuração (ver docs/boot.txt.example)
0:/emu/              os .uf2 dos emuladores (HW16 em /emu/16/)
0:/roms/<sistema>/   suas ROMs
0:/saves/<sistema>/  saves, savestates e .cfg (criados pelo console)
0:/bios/             BIOS do PC Engine CD (necessário para .cue/.chd)
```

Este repositório não contém nem distribui ROMs ou BIOS.

## Binários

Os `.uf2` prontos ficam nos [Releases](../../releases), não no git.

## Documentação

- `AGENTS.md`: arquitetura, decisões e histórico de correções (em português).
- `docs/core2350b_plan.md`: plano da placa Core2350B.
- `docs/boot.txt.example`: todas as chaves do `/boot.txt`.
- `AGENTS.md` dentro de cada emulador: detalhes específicos dele.

## Licença

Cada pasta mantém a licença do seu upstream. Ver [LICENSES.md](LICENSES.md).
