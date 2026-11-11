# Pico8 - Documentação de Integração com pico-bootLoader e Arquitetura PicoDMG

Este documento registra a arquitetura, modificações, mapeamento de memória, protocolo de comunicação com o **pico-bootLoader**, organização do cartão MicroSD, subsistema de áudio I2S e checklist de implementação do projeto **Pico8** (baseado no yocto-8) para o console portátil **PicoDMG** (placas **Waveshare Core2350B HW16** e **RP2350 Plus HW15**).

---

## 1. Visão Geral da Arquitetura

O **Pico8** é um runner bare-metal de alta performance para cartuchos do console de fantasia **PICO-8** no microcontrolador Raspberry Pi RP2350, implementado nativamente em C++20 com aritmética de ponto fixo 16.16 (`libfixmath`) e gerenciamento de memória dual-heap (TLSF com SRAM e PSRAM).

O projeto é padronizado para operar como um emulador integrado (*payload*) dentro do ecossistema do console PicoDMG:
1. **Linkagem em `0x10080000`** via `BootPartition.cmake`, preservando os primeiros 512 KB da Flash para o `pico-bootLoader`.
2. **Saída padronizada de binários UF2**: `Pico8_hw16.uf2` (Core2350B) e `Pico8_hw15.uf2` (RP2350 Plus).
3. **Display SPI0 320x240 (ST7789 ou ILI9341)** com seleção em runtime via `0:/boot.txt` e suporte pleno ao **`GB_MODE`** (janela 266x240 com deslocamento físico x=45..310 para a carcaça do Game Boy DMG).
4. **Controles Físicos Diretos em GP2..GP9** com nível lógico baixo ativo e pull-ups internos.
5. **Integração com `shared_menu`**:
   - Seletor de cartuchos paginado (`dmg_rom_selector`) com Clean Light UI lendo `0:/roms/p8/`.
   - Menu In-Game de pausa (`dmg_ingame_menu`) com Save/Load State, Modos de Escala (Fit, Stretch, Integer), Efeito Grid, Volume, Brilho, Reset e Exit.
   - Combos de sistema em tempo real (`dmg_combo_frame`): `START + B` (menu pausa), `START + A` (toggle FPS), `SELECT + UP/DOWN` (volume HUD), `SELECT + A` (turbo 1x/2x) e `SELECT + START` (retorno imediato ao bootloader).
6. **Handshake com o Bootloader**: detecção de lançamento via `scratch[6] = 0xB007ED01u` e reboot de retorno via `scratch[7] = 0xB007BACEu` + `watchdog_reboot(0, 0, 0)`.
7. **Áudio estéreo I2S (MAX98357A em GP26/GP27)** via PIO DMA a 22.050 Hz com sintetizador de 4 canais executando `sfx()` e `music()`.

---

## 2. Mapa de Memória Flash (16 MB)

Na placa Waveshare RP2350 (16 MB Flash externa W25Q080 / W25Q128), a memória Flash XIP reside de `0x10000000` a `0x11000000`:

```
Endereço XIP      Tamanho     Descrição
-------------------------------------------------------------------------
0x10000000        512 KB      pico-bootLoader (Bootloader residente)
0x10080000        15.5 MB     Partição de Aplicativos (FRENS_APP_BASE)
                              -> Pico8.uf2 é gravado e executado aqui
0x11000000          -         Fim da Flash física / Início da PSRAM
```

### Relocação via `cmake/BootPartition.cmake`
- Gera override de `pico_flash_region.ld` com `ORIGIN = 0x10080000, LENGTH = 0xF80000`.
- Define `PICO_FLASH_SIZE_BYTES = 0x1000000` (16 MB).
- Evita a rejeição `WRONG_LINK_ADDR` durante a gravação pelo bootloader.

---

## 3. Configuração de Hardware e Pinos

| Função | Pinos | Configuração |
| :--- | :--- | :--- |
| **Display TFT (SPI0)** | CS: GP17, SCK: GP18, MOSI: GP19, DC: GP20, RST: GP21, BL: GP22 | SPI a 62.5..72.0 MHz |
| **MicroSD (SPI1)** | MISO: GP12, CS: GP13, SCK: GP14, MOSI: GP15 | FatFs SPI a 20 MHz |
| **Controles (GP2..GP9)** | GP2=Up, GP3=Down, GP4=Left, GP5=Right, GP6=A (🅾️), GP7=B (❎), GP8=Select, GP9=Start | Entrada com Pull-up (ativo em 0) |
| **Áudio I2S** | DATA: GP26, CLK: GP27 | PIO DMA a 22.050 Hz |
| **PSRAM (8 MB SPI)** | HW16: GP47 (Core2350B) / HW15: GP0 (RP2350 Plus) | Mapeada em `0x11000000` |
| **LED Status** | HW16: GP39 / HW15: GP25 | Saída digital |

---

## 4. Pastas do Cartão MicroSD

- **Cartuchos PICO-8**: `0:/roms/p8/` (alternativo: `0:/roms/pico8/`, `0:/roms/PICO8/`)
  - Suporte tanto ao formato texto puro `.p8` quanto ao formato de imagem `.p8.png` (ou `.p8` em formato PNG esteganográfico).
  - Decodificação de imagem RGBA 160x205 via zlib inflate e descompressão de código Lua nativa (`pxa` e `:c:`).
- **Saves e Savestates**: `0:/saves/p8/` (ex: `0:/saves/p8/<jogo>_<slot>.ss`)
- **Configuração Persistente**: `0:/saves/p8/pico8.cfg`
- **Configurações Globais do Console**: `0:/boot.txt` (`ST7789=0/1`, `GB_MODE=0/1`, `ROTATE`, `MADCTL`, `INVERT`)

---

## 5. Modos de Escala de Tela (PICO-8 128x128)

- **Fit**: 240x240 pixels (proporcional 1:1, centralizado no display 320x240 ou na janela DMG 266x240).
- **Stretch**: Tela cheia (320x240 ou 266x240 em `GB_MODE`).
- **Integer**: 128x128 pixels (1x nativo pixel-perfect, centralizado).
- **Grid Effect**: Efeito de scanlines / tela LCD retrô com linhas e colunas alternadas.

---

## 6. Checklist de Progresso e Fases de Implementação

- [x] **Fase 1: Infraestrutura de Build e Offset de Bootloader**
  - [x] Criar `cmake/BootPartition.cmake`.
  - [x] Atualizar `CMakeLists.txt` para incluir HW_CONFIG 16 e 15, `shared_menu` e `frens_offset_for_bootloader`.
  - [x] Criar scripts `build_core2350b.ps1` e `build_for_bootloader.ps1`.

- [x] **Fase 2: Armazenamento MicroSD e Seletor `shared_menu`**
  - [x] Importar `ext/FatFs_SPI` nos pinos 12..15 (SPI1).
  - [x] Substituir o backend de flash FatFS do yocto-8 por cartão SD.
  - [x] Configurar o descritor `s_pico8_desc` apontando para `0:/roms/p8/`.
  - [x] Implementar chamada ao `dmg_rom_selector` antes da inicialização do jogo.

- [x] **Fase 3: Pipeline Gráfico e Driver ST7789/ILI9341 com GB_MODE**
  - [x] Integrar driver `mk_st7789.c` e `dmg_panel.c`.
  - [x] Implementar escalas Fit (240x240), Stretch (320x240 / 266x240) e Integer (128x128 1x).
  - [x] Suporte a `GB_MODE` com janela em x=45..310 e bordas pretas limpas.
  - [x] Blitter assíncrono via DMA em Core 1 com suporte a `park_lcd_core1()`.

- [x] **Fase 4: Controles Físicos GP2..GP9, Combos e Menu In-Game**
  - [x] Mapear GP2..GP9 para os botões virtuais do PICO-8.
  - [x] Integrar `dmg_combo_frame` (START+B, SELECT+START, SELECT+UP/DOWN, START+A, SELECT+A).
  - [x] Conectar o menu de pausa `dmg_ingame_menu` com Save/Load state e submenu de imagem.
  - [x] Handshake com o bootloader via registradores de Watchdog scratch 6 e 7.

- [x] **Fase 5: Áudio I2S e Sintetizador PICO-8 de 4 Canais**
  - [x] Integrar `ext/i2s` (`audio_i2s.pio`, `i2s.c`) em GP26/GP27 via PIO DMA @ 22.050 Hz.
  - [x] Implementar sintetizador de 4 canais decodificando SFX (`0x3200`) e Music (`0x3100`).
  - [x] Conectar bindings Lua `sfx(...)` e `music(...)` ao sintetizador.
  - [x] Integrar controle de volume (0..5) com o HUD OSD.

- [x] **Fase 6: PSRAM e Dual-Heap**
  - [x] Integrar `SetupPsram.c` com CS no GP47 (HW16) e GP0 (HW15).
  - [x] Adicionar os 8 MB de PSRAM à pool TLSF (Heap #2).

- [x] **Fase 7: Validação e Compilação dos Binários UF2**
  - [x] Validar binário HW16 (`Pico8_hw16.uf2`).
  - [x] Validar binário HW15 (`Pico8.uf2` / `Pico8_hw15.uf2`).

---

## Standby por Savestate e Resume (2026-11-05)

Camada comum: `AGENTS.md` da raiz, §45 (`shared_menu/dmg_power`, `dmg_resume`,
`dmg_boot_intro`). Aqui fica so o que mudou neste emulador.

Fluxo: chave OFF -> backlight apagado, painel pintado de preto, savestate no slot
`_R` (`<saves_dir>/<jogo>_R.ss`, nunca aparece no Load State 1..4), saves de
bateria gravados, `0:/saves/resume.bin`, DORMANT. Chave ON -> reboot, o bootloader
pula direto para o emulador, a abertura do bootloader (logo + som) roda no Core 1
enquanto o Core 0 carrega a ROM, e o `_R` e carregado depois do 1o quadro.
Desligar no seletor de jogos volta ao seletor, tambem com a abertura.

### O que foi feito
- `mk_st7789.c`: `mk_ili9225_write_rect_565()` para a abertura.
- Core unico (o Core 1 esta livre para a abertura).
- `flush_saves = NULL`; o state (incluindo o estado Lua) e carregado no primeiro
  `update_button_state()` (`pico8_menu.c`).
- O desenho do menu checa `dmg_boot_intro_active()`: a abertura e dona do painel.

### Audio (`ext/i2s/i2s.c`, `p8_audio.c`)
- Mesmo problema do PicoGBC: `i2s_dma_write()` descarta o que nao cabe no anel, e
  o `audio_free` fixo fazia o som da abertura sair com **tom errado**. Novos
  `i2s_ring_free()` / `i2s_ring_push()`, usados por `p8_audio_intro_free/push`.
- `i2s_audio_start()`/`stop()` limpam a IRQ pendente depois do abort.
- `audio_restore = p8_audio_intro_restore` (`i2s_audio_start()`).

### Status
Validado em hardware (Core2350B) pelo usuario em 2026-11-05: OFF no jogo, no menu
de pausa e no seletor; volta no mesmo ponto, com som e FPS normais.
