# Agents.md — PicoLINX (RP2350 + ILI9341)

Documentação do trabalho realizado e do estado atual do projeto **PicoLINX**.

## Objetivo

Portar o emulador Atari Lynx para Raspberry Pi RP2350 sem PSRAM, com display ILI9341 SPI, menus principal e in-game, quatro save states, efeito de imagem `None | Grid` e organização dos arquivos em `roms/linx` e `saves/linx`.

Artefato esperado:

- `PicoLINX.uf2`

## Estado atual

Pipelines de vídeo e menu portados do **PicoNGP** para o driver **`drivers/ili9341`** nativo SPI (mesma via validada pelo pico-bootLoader), substituindo o pipeline PIO `st7789` que nunca exibiu imagem.

Build validado (RP2350 + ILI9341 + I2S + bootLoader):

- `PicoLYNX.uf2` (380.416 bytes) linkado para pico-bootLoader: BootPartition `ORIGIN=0x10080000 LENGTH=0xf80000`.
- Memória: FLASH 189.788 B (1.17% dos 15.872 KB da partição), RAM 302.052 B (57.61% dos 512 KB, inclui o hot-path do core em `.time_critical`).
- Build via `build_for_bootloader.ps1` → `cmake -DPICO_BOARD=picolinx -DPICO_PLATFORM=rp2350-arm-s -DBUILD_FOR_BOOTLOADER=ON` + `ninja lynx`; UF2 copiado para a raiz.

O projeto atual já contém:

- Core Atari Lynx/Handy em `src/lynx/`.
- Driver `ili9341` (porta do PicoNGP) em `drivers/ili9341/`, com caminho de streaming Lynx embutido (`ili9341_stream_lynx_line`, `ili9341_is_active_lynx_line`, `ili9341_stream_lynx_frame_end`).
- Menu principal/in-game (padrão NGP) e file browser em `src/main.cpp`.
- Save/load via `CSystem::ContextSave()`/`ContextLoad()`, 4 slots persistidos na config.
- Áudio I2S.
- Sem PSRAM: `gPrimaryFrameBuffer`/`SCREEN` vivem em RAM (`uint8_t[102][160]`).

## Configuração atual

`CMakeLists.txt` está configurado para:

- Plataforma: `rp2350`.
- Board: `picolinx` (via build script).
- SDK Pico: 2.3.1.
- Toolchain: 15_2_Rel1.
- Saída em `bin/${CMAKE_BUILD_TYPE}/`.
- `TFT` e `ILI9341` ON; link do driver `ili9341` (bloco `IF(TFT)`); `st7789` e `graphics` removidos do build.
- Nome do executável: `PicoLYNX`.

## Hardware mapeado

O pinout usado é o de `boards/picolinx.h` (spi0 para o TFT — mesma via do pico-bootLoader):

### Display TFT — SPI0

| Sinal | GPIO |
|---|---:|
| CS | 17 |
| SCK | 18 |
| MOSI | 19 |
| DC | 20 |
| RST | 21 |
| BL | 22 |

### SD card — SPI1 (PIO)

| Sinal | GPIO |
|---|---:|
| CS | 15 |
| SCK | 14 |
| MOSI | 13 |
| MISO | 12 |

### Botões (ativos em low)

| Sinal | GPIO |
|---|---:|
| UP | 2 |
| DOWN | 3 |
| LEFT | 4 |
| RIGHT | 5 |
| A | 6 |
| B | 7 |
| START | 8 |
| SELECT | 9 |

### Áudio I2S

| Sinal | GPIO |
|---|---:|
| DATA | 10 |
| CLOCK | 11 |

## Funcionalidades existentes

### File browser

`rom_file_selector()` em `src/main.cpp`:

- Lê diretórios via FatFS (`roms/linx` na raiz do SD via `0:`).
- Filtra extensões `lnx`, `com` e `o` (case-insensitive).
- Ordena diretórios antes dos arquivos; navegação por gamepad.
- Carrega a ROM selecionada para a Flash XIP antes de abrir o core (`load_cart_rom_file`).
- SEL+START = `watchdog_reboot()` (volta ao bootLoader).

### Menu in-game

`show_ingame_menu()` (padrão NGP, texto + `flush_rows`):

- `Save state` (slot 1-4) / `Load state` (slot 1-4).
- `Speed` (1x / 2x turbo).
- `Image >` (Scale Fit/Stretch/Integer, Effect None/Grid, Screen Progressive/Interleave).
- `Volume` (1..5) / `Brightness` (1..5).
- `Reset` (reinicia a ROM) / `Exit` (volta ao jogo).

Combinações:

- START+SELECT = abre o menu.
- START+B = pausa/retoma.
- START+A = alterna exibição de FPS na tela (overlay; não persiste).
- SEL+UP/DOWN = volume.
- Dentro do menu: SEL+START = sair para o selector.
- `Effect None/Grid` agora só é alterado no submenu `Image >` (persistido na config).

### Renderização

`run_game()` em `src/main.cpp`:

- Cria `CSystem` (palette 256 IDX8, áudio 22050 Hz), aplica scale/grid/interleave da config.
- `ili9341_blitter_start()` lança o blitter no core1 (scans por linha, igual ao NGP).
- Loop: lê input (`BUTTON_*` do Lynx), `UpdateFrame(true)`, e faz `ili9341_stream_lynx_line()` para cada uma das 102 linhas de `gPrimaryFrameBuffer` (`frame + l*HANDY_SCREEN_WIDTH`), com checagem de linha ativa (paridade/Stretch/Integer) e depois `ili9341_stream_lynx_frame_end()`.
- DMA I2S via `i2s_dma_write()`; pacing por `busy_wait_us_32` (16.666 µs a 1x, 8.333 µs em turbo).
- FPS calculado por janela de 1 s no loop; overlay verde (START+A) desenhado nas linhas 69..76 do painel.

### Scale (Aspect Ratio)

- `Fit`: uniforme 2x preservando 160x102 (aspecto original) → 320x204 centralizado (18 px de borda preta em cima/baixo).
- `Stretch`: 160→320 horizontal e 102→240 vertical (tela cheia, distorce).
- `Integer`: 160x102 nativo 1:1 em (80,69), sem escala.
- O xmap horizontal da Lynx é próprio (`s_lynx_xmap`), separado do NGP.

## Observações técnicas importantes

1. **Formato de frame:** o core Handy grava **IDX8** (índice 8-bit `mColourMap` 3-3-2) em `gPrimaryFrameBuffer`, pitch 160 bytes/linha (`mDisplayPitch = HANDY_SCREEN_WIDTH * 1`), resolução **160x102** (`system.h:95-96`). `SCREEN` é `uint8_t[102][160]` — não é RGB565.
2. **Palette (originais 8-bit):** `R=((i&0xe0)>>4)*16`, `G=((i&0x1C)>>1)*16`, `B=((i&0x03)<<2)*16`, convertida para RGB565 (`r5=r8>>3, g6=g8>>2, b5=b8>>3`) em `lynx_palette_rgb565[256]`. A conversão é feita no driver (`lynx_band_push`), não no core.
3. **Driver reutiliza infraestrutura do NGP:** `s_fb` RGB565 320x240, slot queue `NES_LCD_SLOTS` (**224**, era 48), dirty-row por hash FNV, blitter core1 `nes_blitter_core1`, xmap 160→320 próprio da Lynx (`lynx_prepare_xmap()`/`s_lynx_xmap`), `s_field_parity`, `s_grid_effect`, `flush_rows`/`fill_screen`/`ili9341_text`/`fill_rect`.
4. **Sem lockout multicore durante load de ROM:** o blitter não roda fora do jogo; o cache em Flash XIP é feito em single-core sem `multicore_lockout_start_blocking()`, com barra de progresso.
5. **Config:** `0:/saves/lynx/picolynx.cfg`, magic `"LNXC"` v1 — campos: scale_mode (0 Fit,1 Stretch,2 Integer), grid_effect, interleave, turbo_mode, volume (1..5), brightness (1..5), save_slot, load_slot. Paths e estados em `0:/saves/lynx/`.
6. **Volume/brightness:** PWM no pino do backlight; nível 5 = alto contínuo. `i2s_volume()` para o áudio (invertido).
7. O módulo PSRAM não é mais usado nem referenciado (`drivers/psram` ausente do CMake).
8. **Gargalo de exibição (diagnóstico):** com 48 slots, o core0 empurra até 204 linhas (Fit) / 240 (Stretch) por frame mas a fila só absorve 48 → `lynx_band_push` bloqueia em `while (s_slot_state[slot])` esperando o SPI do core1 drenar (~71 µs/linha). Resultado: emulação e display **serializados** (~27 ms/frame → ~40 fps ao rolar; parado o dirty-row skip pula tudo e dá 60 fps). Correção: fila dimensionada para caber o frame inteiro → core0 enfileira em ~0,3 ms e o core1 drena durante a emulação do frame seguinte (`__not_in_flash_func`). `NES_LCD_SLOTS = 224` (RAM 415.044 B, 79,16%; heap ~106 KB, ~30 KB de margem sobre os ~76 KB de `new` do `CSystem`).
9. **Overlay de FPS:** START+A mostra apenas o **FPS em 2 dígitos** (não persiste). Toda a instrumentação de profiling usada no diagnóstico (`g_susie_us` em `mikie.cpp`, `g_slot_wait_us`/`g_stream_us`/`g_spi_baud` no driver) foi **removida** depois que a causa raiz foi encontrada.
10. **Conversão Lynx otimizada:** em Fit/Stretch o mapeamento horizontal é sempre `xmap[x]=x>>1` (sem borda), então cada byte-fonte gera 2 pixels-destino idênticos → 1 store de 32 bits por fonte (40 iterações/linha em vez de 320 com lookup de xmap). Fast path em `lynx_band_push` quando `scale != INTEGER` (sem grid e também no caminho com grid). Integer mantém o loop genérico.
11. **Causa raiz das quedas a ~40 fps — clock do SPI:** `clk_peri` seguia o `clk_sys` (360 MHz) e os divisores do PL022 (prescale par × postdiv inteiro) só produzem 180/90/60/45 MHz → o pedido de 72 MHz era **clampado para 60 MHz** (640 B × 8 / 60 MHz ≈ 85 µs/linha, `spi_set_baudrate` no SDK 2.3.1). Portada a técnica do PicoNGP (`src/main.cpp`, logo após `set_sys_clock_khz`): reconfigurar `clk_peri` para **PLL_USB @ 144 MHz** (`pll_init(pll_usb, 1, 1152*MHZ, 4, 2)` + `clock_configure(clk_peri, 0, CLKSRC_PLL_USB, 144 MHz, 144 MHz)`), dando **72 MHz exatos** (divisor 2). Resultado medido pelo usuário: **59/60 fps ao rolar** (era ~40), com `E≈7 P≈0 D≤7`.
12. **Tela de carregamento:** o fundo era `ili9341_clear_black()`; trocado por `ili9341_fill_screen(ROM_UI_BG_COLOR)` (branco, igual ao file browser) em `load_cart_rom_file` — mesmo padrão do PicoNGP.

## Ambiente de compilação localizado

Ferramentas disponíveis localmente:

- Pico SDK: `C:\Users\Michael\.pico-sdk\sdk\2.3.1`
- CMake: `C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin`
- Ninja: `C:\Users\Michael\.pico-sdk\ninja\v1.12.1`
- Toolchain: `C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin`
- Picotool: `C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool`

CMake e Ninja não estão no `PATH`; o build deve usar caminhos absolutos ou o script que configura o ambiente.

## Trabalho realizado

| Etapa | Resultado |
|---|---|
| Inspeção do projeto | Estrutura, fontes, drivers, CMake e documentação de referência mapeados |
| Diagnóstico de tela preta | Identificado pipeline PIO `st7789` não validado; adotada via SPI nativa do PicoNGP/bootLoader |
| Port do driver | `drivers/ili9341` copiado do PicoNGP; caminho Lynx (stream/is_active/frame_end) adicionado |
| Reescrita do main | `src/main.cpp` no padrão NGP (menu, file browser, `run_game`, config, 4 slots) |
| CMake | Link `ili9341` no bloco TFT/ILI9341; `st7789`/`graphics` removidos; `PicoLYNX` mantido |
| Build | `PicoLYNX.uf2` (378.368 B) compilado e confirmado na raiz |
| Ajuste de combos/scale | START+A agora alterna FPS; `Fit` corrigido para 320x204 proporcional; `Grid` só no submenu `Image >` |
| Atualização do AGENTS | Estado, pinos, pipeline e próximas etapas revisados |
| Teste em hardware | Emulador funcional confirmado; ajustes de FPS/Fit pendentes de novo teste |
| Fix do freeze ao sair (Exit) | `~CCart`/`~CRam` faziam `delete[]` em ponteiros que são alias do ROM em Flash XIP → free inválido no `delete lynx`. Destrutores corrigidos para não liberar (cart.cpp:211, ram.h:89) |
| Watchdog no menu | `watchdog_update()` adicionado ao loop do menu in-game (evita reset após >6 s) |
| Build pós-fix | `PicoLYNX.uf2` (380.416 B); FLASH 189.868 B, RAM 262.332 B (50.04%) |
| Performance (core em RAM) | `__time_critical_func` adicionado a todo o hot-path de emulação: `CSusie::PaintSprites` (27.5 KB), `CMikie::UpdateCalcSound`/`Peek`/`Poke`, `CSusie::Peek`/`Poke`, `CSystem::Peek_CPU`/`Poke_CPU` (`system.h`, inline) e os acessos de cart `CCart::Peek0/1/0A/1A`/`Poke0/1/0A/1A`/`CartAddressStrobe`/`CartAddressData`. CPU (`C65C02::Update`), `CMikie::Update` e `CSystem::UpdateFrame` já estavam. Restou em FLASH só caminho frio (ctors/`Reset`/`ContextSave`/`ContextLoad`/`HLE_BIOS`). `PicoLYNX.uf2` (380.416 B); FLASH 189.788 B, RAM 302.052 B (57.61%) |
| Fila de exibição dimensionada ao frame | `NES_LCD_SLOTS` 48 → 224: elimina a serialização emulação×display (core0 não bloqueia mais no `while (s_slot_state[slot])`). `PicoLYNX.uf2` (380.416 B); FLASH 189.884 B, RAM 415.044 B (79,16%) |
| Profiling de Susie + conversão rápida | Overlay passou a mostrar `E`/`S`/`P` (`g_susie_us` em `mikie.cpp` `case CPUSLEEP`). Medição: quedas com `E=7 P=12` → push domina. `lynx_band_push` ganhou fast path 32-bit para Fit/Stretch sem grid (1 store por byte-fonte em vez de 320 com xmap). `PicoLYNX.uf2` (380.928 B); FLASH 190.124 B, RAM 415.248 B (79,20%) |
| **Fix de FPS (causa raiz)** | Desacoplado `clk_peri` do `clk_sys`: reconfigurado para **PLL_USB @ 144 MHz** logo após `set_sys_clock_khz`, permitindo o SPI a **72 MHz exatos** (antes clampava em 60 MHz). Medição do usuário: **59/60 fps** ao rolar. Removida toda a instrumentação; overlay agora só mostra FPS em 2 dígitos. `PicoLYNX.uf2` (382.464 B); FLASH 190.916 B, RAM 415.804 B (79,31%) |
| **Fix do fundo da tela de carregamento** | `load_cart_rom_file`: `ili9341_clear_black()` → `ili9341_fill_screen(ROM_UI_BG_COLOR)` (branco, padrão NGP). `PicoLYNX.uf2` (382.464 B) |

## Fase HW_CONFIG 16 — Waveshare Core2350B (RP2350B + 8 MB PSRAM + ST7789)

Port do PicoLINX para o console, seguindo o padrão do PicoNES e do PicoPCE: carga
e execução dos cartuchos na PSRAM, painel ST7789 escolhido em runtime, e o
`HW15`/`ILI9341` original preservado sem regressão.

> **Nota de arquitectura:** o PicoLINX é um projeto **Murmulator** e não um
> projeto `pico_shared`. Usa `boards/*.h` + `PICO_BOARD`, tem os seus próprios
> drivers em `drivers/`, e **não** tem `pico_shared/BoardConfigs.cmake`, nem
> `Frens::`, nem `pico_lib`. Por isso o port **não** pode ser uma cópia do
> `HW_CONFIG 16` dos outros emuladores. Decidido: reutilizar o `pico_shared`
> apenas nas peças que faltam (QMI PSRAM, lwmem, `dmg_panel`), mantendo o driver
> `drivers/ili9341` nativo, que já é um **superset** (NES, PCE, MD, NGP e Lynx).

### Binários

| Alvo | Ficheiro | Tamanho | FLASH | RAM | Validação |
| :--- | :--- | ---: | ---: | ---: | :--- |
| **Core2350B** | `PicoLNX_hw16.uf2` | 423.424 B | 211.292 B (1,30%) | 429.336 B (81,89%) | compila limpo; `SetupPsram`, `s_rom_ptr` e `lwmem_malloc_ex` presentes no ELF |
| **RP2350 Plus** | `PicoLNX.uf2` | 420.864 B | 210.092 B (1,29%) | 428.484 B (81,73%) | compila limpo; `SetupPsram` **ausente**, `lynx_load_rom_to_psram` removida pelo compilador |

Builds: `build_core2350b.ps1` (HW16) e `build_for_bootloader.ps1` (HW15).
**Diretório de saída por placa** — `bin/core2350b/Release/` e `bin/picolinx/Release/`.

### O que foi feito

1. **`boards/core2350b.h`** (novo). O pinout de `picolinx.h` **já coincidia** com
   o do console (SD SPI1 12-15, TFT SPI0 17-22, áudio 26/27, botões 2-9), porque
   foi copiado do PicoNGP. A diferença são só duas coisas: **PSRAM em GP47** e
   **LED em GP39**. O áudio I2S deriva o LRCLK de `AUDIO_CLOCK_PIN + 1`, logo
   `AUDIO_CLOCK_PIN 27` já dá BCLK 27 / LRCLK 28 — sem alteração.
2. **Stack QMI de PSRAM.** `drivers/psram/psram_spi.c` (PIO SPI) **não serve**:
   a PSRAM do Core2350B está no barramento QSPI/XIP e tem de ser conduzida por
   QMI. Copiados `drivers/pico_psram/` e `drivers/lwmem/` do `pico_shared` do
   PicoNGP, ligados via `LWMEM_OPTS_FILE` + `add_subdirectory`.
   `PicoPlusPsram.cpp` foi alterado para guardar a detecção em `#if PSRAM_CS_PIN`
   — `SetupPsram()` faz `gpio_set_function(csramCS, GPIO_FUNC_XIP_CS1)` sem
   condição, e com o pino 0 isso roubava o GP0 e deixava o motor QMI direct-mode
   ligado no HW15.
3. **Painel em runtime.** `ili9341_init()` foi dividida em
   `st7789_init_sequence()` e `ili9341_init_sequence()`, escolhidas por
   `dmg_panel_get()`. O `MADCTL` deixou de ser hardcoded (`0x28`) e vem de
   `dmg_panel_get_madctl()`. `dmg_panel.c` foi acrescentado às fontes, e
   `FF_USE_STRFUNC` passou a `1` em `drivers/fatfs/ffconf.h` porque o parser de
   `/boot.txt` usa `f_gets()` (o resto é recolhido pelo linker).
   A tabela de painel é a mesma do PicoPCE/PicoNES: ILI9341 `0x28`/`0xE8`,
   ST7789 `0x60`/`0xA0`, com `XOR 0xC0` no 180°.
4. **Carga na PSRAM.** `s_rom_ptr` + `s_rom_in_psram` substituem a constante
   `rom`. `lynx_load_rom_to_psram()` lê o cartucho do SD directo para a PSRAM em
   blocos de 16 KB, com barra de progresso e `watchdog_update()`. A flag é a
   fonte única de verdade: a janela da PSRAM está em `0x11000000`, que nenhuma
   comparação de endereço detecta de forma portátil. A imagem é **byte-idêntica**
   ao caminho de flash; o core endereça `s_rom_ptr` só por aritmética sobre o
   endereço emulado.
   O caminho de flash mantém-se e liberta qualquer buffer de PSRAM anterior, para
   que os dois caminhos nunca deixem um `s_rom_ptr` pendurado.
   **Não** é preciso `apply_safe_qmi_timing()` aqui: ele corrige o divisor do
   Flash XIP (M0) e a leitura de PSRAM não passa por lá.

### Bug corrigido: escrita fora dos limites nos banks Audin do `CCart`

`cart.cpp` calculava `cartsize` (o que sobra depois de bank0 e bank1) e **nunca
o usava**: os ponteiros dos banks A eram postos incondicionalmente.

```c
if (CartGetAudin()) {
   mCartBank0A = gamedata+(bank0size+bank1size);   // sem verificação
   cartsize = __max(0, cartsize - bank0size);
   mCartBank1A = gamedata+(bank0size+bank1size+bank0size);
```

`Suzie` escreve via `Poke0`/`Poke1` (`susie.cpp:1484`), que escrevem
`mCartBank1[addr & mMaskBank1]`. Um jogo que declara Audin mas traz o cart
pequeno — ou cujos banks somam a imagem inteira — escrevia **fora da alocação**.

Na janela Flash XIP isso era inofensivo: stores em flash são silenciosamente
descartados. Com a ROM na PSRAM, o mesmo código passa a ser uma escrita real
fora dos limites, **corrompendo a metadata de blocos do próprio lwmem**. É a
mesma classe de bug do underflow de `optimise_blocks` (PicoNGP) e do desvio de
512 B do cabeçalho SMD (PicoMD).

Corrigido com `if (cartsize > 0)` antes de cada bank A. Deixar os banks A a
`NULL` é exactamente o que o caminho de leitura da 6502 já trata como "sem
Audin": `Poke0`/`Poke1` ficam no-ops e `Peek0A` devolve `0xFF` — melhor do que
corromper o alocador, e é o que um Lynx real sem EEPROM faz.

### Bug de build corrigido: um alvo sobrescrevia o ELF do outro

`OUTPUT_DIR` era `bin/${CMAKE_BUILD_TYPE}` para as duas placas, então o último
build configurado sobrescrevia o `.elf`/`.map` do outro. É a mesma classe de bug
do §24.6 do `PicoDMG-Temp/AGENTS.md` (no PicoGBC, `build_hw16` sobrescrevia o
binário HW15 com o mesmo nome). Agora é `bin/<placa>/<build>/`.

Os dois scripts de build também passaram a validar `$LASTEXITCODE` e a recusar
um `.uf2` com mais de 5 minutos — um `ninja` que falha deixa o `.uf2` anterior
no lugar, e `Test-Path` sozinho copiava binário velho imprimindo `[OK]`.

### Validação pendente em hardware

Nada disto foi testado na placa. Por ordem de importance:

1. **`ST7789=1` em `/boot.txt`.** O binário funciona sem `/boot.txt` thanks ao
   `DEFAULT_PANEL_ST7789=1` embutido, mas isso **só cobre o cartão sem
   `/boot.txt`**. Com um `/boot.txt` presente e sem a chave `ST7789=`, o painel
   cairia no default ILI9341 e a imagem sairia espelhada (é o `MX` a menos do
   `0x60`) — o mesmo ponto que ficou registado como pendente no PicoNES e no
   PicoPCE.
2. **Detecção da PSRAM em GP47** e o log `[P] Cart in PSRAM: <n> B at <ptr>`.
3. **Jogos que declaram Audin/EEPROM.** As escritas agora acontecem em RAM. Não
   há `.sav` de bateria/flash no PicoLINX, portanto as escritas de EEPROM
   continuam a ser perdidas ao sair — não há regressão (antes eram descartadas
   na flash), mas é uma functionality em falta. Confirmar que nenhum jogo
   depende de ler de volta uma escrita que antes era silenciosamente perdida.
4. **FPS**: o baseline medido é 59/60 com SPI a 72 MHz. A ROM na PSRAM é
   *cached* (`0x11000000`), não o alias não-cacheado `0x15000000` — daí não
   haver motivo para regressão, mas convém confirmar.

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
- `ili9341_write_rect_565()` no driver.
- Audio deste emulador e por quadro e bloqueante (`i2s_dma_write`, 22,05 kHz), sem
  anel. A abertura (44,1 kHz) e decimada 2:1 em `lynx_intro_audio_push()` e enviada
  em blocos de 1 quadro; `lynx_intro_audio_free()` responde 64 (a escrita bloqueia).
  `audio_restore = lynx_audio_restore`: espera o ultimo DMA e descarta o bloco
  parcial, para o 1o `i2s_dma_write()` do jogo comecar com o DMA ocioso.
- `flush_saves = SaveEEPROM()`.
- Gravacao da ROM em flash: compara antes de apagar.
- `_R` carregado em `run_game()`, dentro do loop, depois do 1o quadro, pela
  sequencia do Load State do menu de pausa.

### Correcao: chave e indicador de bateria sem efeito
`boards/core2350b.h` incluia `boards/pico2.h`, que e **RP2350A** (`PICO_RP2350A 1`,
30 GPIOs, `ADC_BASE_PIN 26`). Pino da chave >= 30 e GP40 (bateria) ficavam
inacessiveis; confirmado no ELF (faixa de ADC aceita 26..29). Agora inclui
`boards/waveshare_core2350b.h` (RP2350B, 48 GPIOs, ADC 40..47), com
`#undef PICO_FLASH_SIZE_BYTES` antes da redefinicao propria.

### Status
Validado em hardware (Core2350B) pelo usuario em 2026-11-05: OFF no jogo, no menu
de pausa e no seletor; volta no mesmo ponto, com som e FPS normais.
