# PicoDMG - DocumentaÃ§Ã£o de Arquitetura e UnificaÃ§Ã£o de Menus

Este documento registra a organizaÃ§Ã£o do projeto **PicoDMG-Temp**, a estratÃ©gia de unificaÃ§Ã£o de menus e o progresso das alteraÃ§Ãµes entre os emuladores suportados.

---

## 1. Emuladores Suportados no Console

O projeto consolida 9 emuladores para o console portÃ¡til baseado em **Raspberry Pi RP2350** (placa **Waveshare RP2350 Plus 16MB**) com display SPI **ILI9341** (320x240) e Ã¡udio I2S (MAX98357A):

| Emulador | Pasta | TÃ­tulo | ExtensÃµes | Pasta de ROMs PadrÃ£o | ConfiguraÃ§Ã£o Salva |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **PicoA26** | `C:\dev\PicoDMG-Temp\PicoA26` | `PicoA26` | `.a26`, `.bin` | `0:/roms/a26` | `0:/saves/a26/picoa26.cfg` |
| **PicoGBC** | `C:\dev\PicoDMG-Temp\PicoGBC` | `PicoGBC` | `.gb`, `.gbc` | `0:/roms/gbc` | `0:/saves/gbc/picogbc.cfg` |
| **PicoLINX** | `C:\dev\PicoDMG-Temp\PicoLINX` | `PicoLYNX` | `.lnx`, `.o` | `0:/roms/lynx` | `0:/saves/lynx/picolynx.cfg` |
| **PicoMD** | `C:\dev\PicoDMG-Temp\PicoMD` | `PicoMD` | `.md`, `.bin`, `.gen`, `.smd` | `0:/roms/md` | `0:/saves/md/picomd.cfg` |
| **PicoNES** | `C:\dev\PicoDMG-Temp\PicoNES` | `PicoNES` | `.nes`, `.fds`, `.nsf` | `0:/roms/nes` | `0:/saves/nes/picones.cfg` |
| **PicoNGP** | `C:\dev\PicoDMG-Temp\PicoNGP` | `PicoNGP` | `.ngp`, `.ngc` | `0:/roms/ngp` | `0:/saves/ngp/picongp.cfg` |
| **PicoPCE** | `C:\dev\PicoDMG-Temp\PicoPCE` | `PicoPCE` | `.pce`, `.sgx` | `0:/roms/pce` | `0:/saves/pce/picopce.cfg` |
| **PicoSMS** | `C:\dev\PicoDMG-Temp\PicoSMS` | `PicoSMS` | `.sms`, `.gg`, `.sg` | `0:/roms/sms` | `0:/saves/sms/picosms.cfg` |
| **PicoWS** | `C:\dev\PicoDMG-Temp\PicoWS` | `PicoWS` | `.ws`, `.wsc` | `0:/roms/ws` | `0:/saves/ws/picows.cfg` |
| **PicoSNES** | `C:\dev\PicoDMG-Temp\PicoSNES` | `PicoSNES` | `.smc`, `.sfc`, `.bin`, `.fig` | `0:/roms/snes` | `0:/saves/snes/picosnes.cfg` |

*(Nota: SNES-Recomp e PicoGBA foram excluídos conforme instrução do usuário. O PicoGBA é standalone e não é viável nesta placa; o `pico-bootLoader` aceita apenas imagens Arm.)*

---

## 2. DiagnÃ³stico da SituaÃ§Ã£o Atual

### O Problema da DuplicaÃ§Ã£o
Atualmente, cada emulador possui uma cÃ³pia idÃªntica (entre 800 e 1.200 linhas) de cÃ³digo de interface de usuÃ¡rio diretamente em seu `main.cpp` (ou `main.c` no caso do PicoGBC):
- DefiniÃ§Ãµes da paleta **Clean Light UI** (`ROM_UI_BG_COLOR`, `ROM_UI_BAR_COLOR`, etc.).
- Leitura de botÃµes nos GPIOs 2 a 9 (`read_ui_buttons()`, `wait_for_all_ui_buttons_released()`).
- FunÃ§Ãµes de renderizaÃ§Ã£o de barras: `draw_ingame_top_bar()`, `draw_ingame_bottom_bar()`, `draw_ingame_game_bar()`, `draw_ingame_status()`.
- Seletor de ROMs paginado com 9 itens (`rom_file_selector_display_page()`, `rom_file_selector()`).
- Tela de carregamento/gravaÃ§Ã£o em Flash (`draw_clean_light_loading_screen()`).
- Menu In-game de pausa (`ingame_menu()`) com Save/Load state (slots 1..4), ajuste de volume, brilho, reset e exit.

### ConsequÃªncias
1. Qualquer melhoria visual, ajuste de layout ou correÃ§Ã£o de bug exige alterar manualmente 9 arquivos diferentes.
2. Foram criados scripts PowerShell com substituiÃ§Ãµes regex (`patch_all.ps1`, `fix_picomd.ps1`, etc.) que sÃ£o frÃ¡geis e quebram facilmente quando o cÃ³digo ao redor muda.
3. Risco alto de divergÃªncia de comportamento entre emuladores.

---

## 3. Arquitetura da SoluÃ§Ã£o Unificada (`shared_menu`)

Criar uma biblioteca compartilhada em `C:\dev\PicoDMG-Temp\shared_menu\`:

```
C:\dev\PicoDMG-Temp\
â”œâ”€â”€ shared_menu/
â”‚   â”œâ”€â”€ CMakeLists.txt              # MÃ³dulo CMake opcional / interface library
â”‚   â”œâ”€â”€ dmg_menu.h                  # Header Ãºnico C/C++ com API e estruturas
â”‚   â”œâ”€â”€ dmg_menu.c                  # LÃ³gica unificada do menu, seletor de ROMs e UI
â”‚   â”œâ”€â”€ dmg_menu_config.h           # Estruturas de configuraÃ§Ã£o persistente
â”‚   â””â”€â”€ dmg_display_adapter.h       # Interface mÃ­nima de desenho (ILI9341 / ILI9225)
â”œâ”€â”€ PicoA26/
â”œâ”€â”€ PicoGBC/
â”œâ”€â”€ PicoLINX/
â”œâ”€â”€ PicoMD/
â”œâ”€â”€ PicoNES/
â”œâ”€â”€ PicoNGP/
â”œâ”€â”€ PicoPCE/
â”œâ”€â”€ PicoSMS/
â””â”€â”€ PicoWS/
```

### Contrato de Dados do Emulador (`dmg_emulator_desc_t`)
Cada emulador apenas instancia e passa uma estrutura descritora para a biblioteca:

```c
typedef struct {
    const char *app_title;            /* Ex: "PicoNES" */
    const char *default_rom_dir;      /* Ex: "0:/roms/nes" */
    const char **candidate_dirs;      /* Lista de pastas alternativas */
    const char *config_file_path;     /* Ex: "0:/saves/nes/picones.cfg" */
    uint32_t   config_magic;          /* Magic do arquivo .cfg */
    
    /* ValidaÃ§Ã£o de arquivo de ROM */
    bool (*is_rom_file)(const char *filename);
    
    /* Callbacks de aÃ§Ãµes do emulador */
    bool (*save_state)(uint8_t slot);
    bool (*load_state)(uint8_t slot);
    void (*apply_scale)(int scale_mode);
    void (*apply_effect)(bool effect_on);
    void (*apply_interleave)(bool interleave_on);
    void (*apply_speed)(bool turbo_on);
    void (*apply_volume)(uint8_t volume);
    void (*apply_brightness)(uint8_t brightness);
    void (*reset_game)(void);
    void (*exit_game)(void);
    
    /* Submenu personalizado opcional (ex: Paletas DMG no GBC, 3/6 botÃµes no MD) */
    void (*custom_submenu)(const char *game_name);
    const char *custom_submenu_title; /* Ex: "Palette >", "Controller >" */
} dmg_emulator_desc_t;
```

---

## 4. Plano de ImplementaÃ§Ã£o e Checklist de Progresso

- [x] **Fase 1: Desenho e EspecificaÃ§Ã£o do MÃ³dulo Compartilhado**
  - [x] Levantamento de todas as variaÃ§Ãµes entre os 9 emuladores.
  - [x] CriaÃ§Ã£o deste documento `agents.md`.
  - [x] ElaboraÃ§Ã£o do `implementation_plan.md` detalhado.
  - [x] AprovaÃ§Ã£o do usuÃ¡rio.

- [x] **Fase 2: ConstruÃ§Ã£o do MÃ³dulo `shared_menu`**
  - [x] Implementar `dmg_display.h` abstraindo chamadas grÃ¡ficas (`fill_rect`, `draw_text`, `flush_rows`, `flush`, `stop_blitter`).
  - [x] Implementar `dmg_menu.h` e `dmg_menu.c` contendo o seletor de ROMs e menu in-game.
  - [x] Implementar leitura de controles (GPIO 2..9) e persistÃªncia de `.cfg`.

- [x] **Fase 3: Piloto com Primeiro Emulador (PicoNES)**
  - [x] Atualizar `CMakeLists.txt` do emulador para incluir `shared_menu`.
  - [x] Substituir o bloco duplicado em `main.cpp` pela chamada descritora `s_nes_desc`.
  - [x] Compilar com `build_for_bootloader.ps1` e verificar binÃ¡rio `.uf2` (`PicoNES.uf2` gerado).

- [x] **Fase 4: ExpansÃ£o para os Demais Emuladores**
  - [x] **PicoSMS & PicoGG** (`PicoSMS.uf2` e `PicoGG.uf2` gerados e validados)
  - [x] **PicoGBC & PicoDMG** (`PicoGBC.uf2` e `PicoDMG.uf2` gerados e validados)
  - [x] **PicoMD** (`PicoMD.uf2` gerado e validado)
  - [x] **PicoA26** (`PicoA26.uf2` gerado e validado)
  - [x] **PicoLINX** (`PicoLNX.uf2` gerado e validado)
  - [x] **PicoNGP** (`PicoNGP.uf2` gerado e validado)
  - [x] **PicoPCE** (`PicoPCE.uf2` gerado e validado)
  - [x] **PicoWS** (`PicoWS.uf2` gerado e validado)

- [x] **Fase 5: Limpeza e ValidaÃ§Ã£o**
  - [x] Remover scripts PowerShell redundantes de patching regex (`patch_all.ps1`, etc.).
  - [x] Compilar e validar todos os 9 emuladores (11 binÃ¡rios UF2 gerados).
  - [x] Corrigir bug de buffer desalinhado e clock SPI excessivo no `pico-bootLoader` (`s_read_buf` em `0x2003197D`, erro `UF2_DIAG_CORRUPT`). Recompilado `pico-bootLoader.uf2` para ILI9341.

---

## 5. EspecificaÃ§Ã£o do Modo Game Boy DMG (`GB_MODE` - Janela 266x240 com Shift FÃ­sico)

### DiagnÃ³stico de Alinhamento da CarcaÃ§a DMG
Devido ao posicionamento fÃ­sico do display ILI9341 dentro do corte da carcaÃ§a original do Game Boy ClÃ¡ssico (DMG), a janela visÃ­vel **nÃ£o Ã© matematicamente centralizada**: ela possui um deslocamento horizontal (`GB_RENDER_X_SHIFT = 18u`).
- **ResoluÃ§Ã£o do display**: 320x240 (x=0..319, y=0..239).
- **Largura da janela visÃ­vel**: 266 pixels (`GB_WINDOW_W = 266`).
- **Altura da janela visÃ­vel**: 240 pixels (`GB_WINDOW_H = 240`).
- **PosiÃ§Ã£o X inicial**: `GB_WINDOW_X = ((320 - 266) / 2) + 18 = 27 + 18 = 45`.
- **Janela DMG ativa**: `x = 45 .. 310` (largura 266), `y = 0 .. 239`.
- **Borda esquerda (preta)**: `x = 0 .. 44` (**45 pixels**).
- **Borda direita (preta)**: `x = 311 .. 319` (**9 pixels**).

### ConfiguraÃ§Ã£o via `0:/boot.txt`
- `GB_MODE=0` (padrÃ£o quando ausente ou 0): Tela cheia 320x240 em todos os menus, bootloader e emuladores.
- `GB_MODE=1`: Modo janela DMG (largura 266 de x=45 a x=310, y=0 a y=239), com bordas pretas assimÃ©tricas (45px na esquerda, 9px na direita).

### Regras de Escala In-Game para `GB_MODE=1`:
- **Fit**: Escala a resoluÃ§Ã£o nativa do emulador para caber dentro da janela 266x240 (x=45..310) mantendo o aspecto da imagem original (PAR/DAR).
- **Stretch**: Escala a resoluÃ§Ã£o nativa para preencher completamente a janela 266x240 (x=45..310, y=0..239).
- **Integer**:
  - Se a resoluÃ§Ã£o nativa for **menor** que 266x240 (ex: GB/GBC 160x144, SMS 256x192, GG 160x144, Lynx 160x102, NGP 160x152, WonderSwan 224x144, NES 256x240): opÃ§Ã£o presente no menu, exibindo em escala 1x (ou nx inteira) centralizada na janela DMG (offset inicial em x=45).
  - Se a resoluÃ§Ã£o nativa for **maior** que 266x240 (ex: Mega Drive 320x224, PC Engine 320x240 ou 512x240): a opÃ§Ã£o Integer **nÃ£o existe** no menu in-game, alternando apenas entre **Fit** e **Stretch**.

### Regras de Escala In-Game para `GB_MODE=0`:
- MantÃ©m o comportamento e menus exatamente como estÃ£o atualmente (320x240, opÃ§Ãµes Fit, Stretch e Integer padrÃ£o).

---

## 6. Plano de ExecuÃ§Ã£o `GB_MODE`

- [x] **Etapa 1: Suporte a `GB_MODE` no Bootloader (`pico-bootLoader`)**
  - [x] Atualizar `sd_boot_ini.h` e `sd_boot_ini.c` para reconhecer e salvar `GB_MODE=0` e `GB_MODE=1`.
  - [x] Adaptar o driver ILI9341 e rotinas de renderizaÃ§Ã£o do bootloader (`main.cpp`, `progress_bar.cpp`, `gui.cpp`) para renderizar dentro da janela 266x240 em x=45..310 com bordas pretas (0..44 e 311..319) quando `GB_MODE=1`.
  - [x] Testar e compilar `pico-bootLoader.uf2`.

- [x] **Etapa 2: Suporte a `GB_MODE` na Biblioteca Unificada (`shared_menu`)**
  - [x] Implementar leitura de `0:/boot.txt` em `shared_menu` (`dmg_is_gb_mode()`, `dmg_get_viewport()`).
  - [x] Adaptar geometria das barras (top, bottom, game, status), seletor de ROMs paginado, tela de progresso/carregamento flash e menu in-game para respeitar os limites de 266x240 (x=45..310) e limpar bordas laterais quando ativo.

- [x] **Etapa 3: AdaptaÃ§Ã£o das Regras de Escala e Submenus dos Emuladores**
  - [x] **PicoGBC & PicoDMG**: Ajustar driver `mk_ili9341.c` para usar o offset correto x=45 (substituindo o antigo 27u) e menu de display (Fit, Stretch e Integer dentro de x=45..310).
  - [x] **PicoNES**: Ajustar driver `pico_shared/drivers/ili9341/ili9341.c` (`nes_prepare_band_xmap`) e submenu Image (Fit, Stretch, Integer).
  - [x] **PicoSMS & PicoGG**: Ajustar driver `ili9341.c` e submenu Display (Fit, Stretch, Integer).
  - [x] **PicoMD**: Ajustar driver `ili9341.c` e submenu Display: modo 320x224 oculta Integer (somente Fit/Stretch); modo 256x224 permite Integer se aplicÃ¡vel.
  - [x] **PicoPCE**: Ajustar driver `ili9341.c` e submenu Display: resoluÃ§Ãµes > 266 ocultam Integer (somente Fit/Stretch).
  - [x] **PicoLINX**: Ajustar driver `ili9341.c` e submenu Display (Fit, Stretch, Integer).
  - [x] **PicoNGP**: Ajustar driver `ili9341.c` e submenu Display (Fit, Stretch, Integer).
  - [x] **PicoWS**: Ajustar driver `ili9341.c` e submenu Display (Fit, Stretch, Integer).
  - [x] **PicoA26**: Ajustar blitter `ili9341.c` e submenu Image (Fit, Stretch, Integer).

- [x] **Etapa 4: ValidaÃ§Ã£o Geral e CompilaÃ§Ã£o dos BinÃ¡rios UF2**
  - [x] Recompilar todos os alvos e verificar ausÃªncia de warnings ou regressÃµes (12 binÃ¡rios UF2 gerados com sucesso).
  - [x] Validar comportamento com `GB_MODE=0` (320x240 tela cheia) e com `GB_MODE=1` (266x240 na janela DMG x=45..310 com padding preto 45px/9px).

---

## 7. CorreÃ§Ãµes de Teste em Hardware (GB_MODE=1)

| Emulador | Sintoma Original | Causa Raiz Identificada | CorreÃ§Ã£o Aplicada | BinÃ¡rios UF2 |
| :--- | :--- | :--- | :--- | :--- |
| **shared_menu** | Menus travavam por watchdog apÃ³s ~6s de inatividade | `dmg_init_boot_mode` travava `s_boot_mode_read = true` antes de SD montar; loops de menu sem `watchdog_update()` | `s_boot_mode_read` apenas em `FR_OK` (tentando caminhos mÃºltiplos); `watchdog_update()` em todos os loops | N/A (compartilhado) |
| **PicoGBC / PicoDMG** | Menu em tela cheia (320x240) e jogo nÃ£o deslocado para x=45 | `dmg_init_boot_mode` falhava antes de `sd_storage_ready()`, e `mk_ili9225_set_scale_mode` ignorava redimensionamento | `mk_st7789.c` e `mk_ili9341.c` rastreiam `s_last_gb_mode` e recalculam coordenadas com `DMG_GB_WINDOW_X = 45` | `PicoGBC.uf2`, `PicoDMG.uf2` |
| **PicoSMS / PicoGG** | Jogos nÃ£o abriam apÃ³s seleÃ§Ã£o no menu | `flashrom()` tentava abrir nome de arquivo relativo diretamente sem prefixar a pasta ativa | ConstruÃ§Ã£o de caminho completo via `dmg_get_current_rom_dir()` | `PicoSMS.uf2`, `PicoGG.uf2` |
| **PicoLINX** | "No ROM files found! Folder: 0:/roms/lynx is empty" | Montava volume `""` em vez de `"0:"`; filtro de extensÃµes nÃ£o incluÃ­a `.lyx` e `.bin` | `f_mount(&fs, "0:", 1)`; inclusÃ£o de extensÃµes `.lyx`, `.bin`, `.zip`; ajuste de diretÃ³rios candidatos | `PicoLNX.uf2` |
| **PicoMD** | Jogo iniciava em tela cheia; crash ao mudar para Fit; nÃ£o abria mais jogos; retorno ao menu ou tela vermelha (red screen) | Fast path 320x240 ignorava `GB_MODE`; retorno do pause menu nÃ£o reativava blitter; SD SPI overclockado a 45 MHz (`CLK_FAST_DEFAULT=50MHz` c/ `clk_peri=360MHz`); cabeÃ§alho copier SMD 512B deslocava vetores | Bloqueio de fast path em `GB_MODE`; reativaÃ§Ã£o de blitter pÃ³s-menu; `clk_peri` desacoplado a 140 MHz via PLL_USB e `CLK_FAST_DEFAULT=20MHz` (SD seguro a 17.5 MHz); skip de cabeÃ§alho 512B | `PicoMD.uf2` |
| **PicoNGP** | Congelamento de vÃ­deo ao sair do menu in-game (Ã¡udio continuava tocando); crash para lista de jogos | `ili9341_blitter_start()` ausente ao retomar jogo apÃ³s pause menu; falta de `watchdog_update()` | ReativaÃ§Ã£o de blitter pÃ³s-menu; sincronizaÃ§Ã£o de diretÃ³rio de ROMs; alimentaÃ§Ã£o de watchdog | `PicoNGP.uf2` |
| **PicoPCE** | BotÃµes A e B nÃ£o funcionavam; Select parecia agir como botÃ£o de aÃ§Ã£o | Multitap cycling (`counter` 0..4 em `0x1000`) lia registradores 1..4 zerados (sem botÃ£o ativo); turbo zerava A e B | Espelhamento de inputs do P1 em `regs[0..4]`; remoÃ§Ã£o de mÃ¡scara indevida de A/B no turbo; `memset` de Joypad no reset | `PicoPCE.uf2` |
| **FPS Overlay (Geral)** | Ao apertar Start + A, o contador de FPS nÃ£o aparecia na tela no modo `GB_MODE=1` | Texto desenhado em `x_pos = 8` (Ã¡rea da borda preta `0..44` coberta pelo plÃ¡stico da carcaÃ§a do Game Boy); bug de escopo em variÃ¡vel estÃ¡tica de combo no PicoMD | Ajuste de coordenadas para `x_pos = dmg_is_gb_mode() ? 53 : 8` nos drivers ILI9341; correÃ§Ã£o do toggle de `START + A` com detecÃ§Ã£o limpa de borda | `PicoMD.uf2`, `PicoSMS.uf2`, `PicoGG.uf2`, `PicoLNX.uf2`, `PicoWS.uf2` |
| **PicoMD (Crash/Exit)** | Jogos (Comix Zone, Sonic 2) fechavam sozinhos apÃ³s alguns segundos de gameplay | 1) Overclock da Flash XIP a 180 MHz no RP2350 (CPU a 360 MHz c/ divisor QMI padrÃ£o 2 em vez de 4 @ 90 MHz; `boot2` resetava divisor ao gravar flash); 2) GP8 (SELECT) mapeia para BotÃ£o C (Pulo/AÃ§Ã£o), fazendo `combo_exit` disparar instantaneamente sem hold ao pular/bater | 1) `apply_safe_qmi_timing()` fixa divisor QMI em 4 (90 MHz) no boot, pÃ³s-flash e no gameplay; 2) `SELECT + START` exige retenÃ§Ã£o contÃ­nua de 60 frames (~1s); 3) `gwenesis_io_get_buttons()` desacoplado para nÃ£o ler GPIOs dentro do barramento 68k | `PicoMD.uf2` |
| **PicoNGP (Crash/Exit/Scale)** | Ao mudar escala no menu in-game encerrava/rebootava; travamentos esporÃ¡dicos durante gameplay | 1) Overclock da Flash XIP a 180 MHz no RP2350 (CPU @ 360 MHz sem divisor QMI); 2) `ngp_image_submenu()` gravava SD card a cada clique com clock SD SPI excessivo (35 MHz) e sem `watchdog_update()`, causando timeout do watchdog (6s); 3) Combo `SELECT + START` sem hold delay | 1) `apply_safe_qmi_timing()` fixa divisor QMI em 4 (90 MHz); 2) ReduÃ§Ã£o de `CLK_FAST_DEFAULT` para 20 MHz (SD seguro a 17.5 MHz) e adiamento da gravaÃ§Ã£o do `.cfg` para a saÃ­da do menu (tecla B) com `watchdog_update()`; 3) Hold de 60 frames para `SELECT + START` | `PicoNGP.uf2` |
| **PicoWS (Trava no Carregamento / Crash PÃ³s-Flash)** | Jogos travavam na tela de flash ou tela preta e reiniciavam de volta para a lista de jogos | 1) `audio_i2s_setup(USE_I2S_AUDIO, 44100, -1)` nÃ£o era chamado no `main()`, deixando `audio_ring == NULL`; na linha 0 do frame 1, `audio_i2s_enqueue_samples` acessava ponteiro nulo (0x0) causando `HardFault` imediato; 2) Divisor QMI Flash resetado para 2 pÃ³s-flash; 3) `ws_patchRom` tentava escrever na Flash (`rom[0xfffe8]=0xea`); 4) Objetos do NEC V30 estavam em Flash no build Linux por falta do padrÃ£o `.o` | 1) InclusÃ£o de `audio_i2s_setup` e verificaÃ§Ã£o contra `NULL` em `audio_i2s_enqueue_samples`; 2) Safe Flash wrappers com divisor QMI 4 (90 MHz); 3) Patch virtual de Conan em `cpu_readmem20` sem store na Flash; 4) Mapeamento de `nec.c.o`, `ws.c.o`, `memory.c.o` para SRAM (496 KB / 94.7%) | `PicoWS.uf2` |
| **PicoNGP (Ãudio Ausente / Mudo)** | Jogos ficavam sem Ã¡udio durante o gameplay | 1) `sample_chip_tone` e `sample_chip_noise` geravam amostras assimÃ©tricas (0..32767) e `update_audio_frame` subtraÃ­a erroneamente `16384`, introduzindo um deslocamento DC massivo constante (-9856) no silÃªncio/gameplay que saturava e desarmava o amplificador I2S MAX98357A; 2) AusÃªncia de centralizaÃ§Ã£o AC simÃ©trica `(2*vol - STEP) * Volume` | 1) GeraÃ§Ã£o de formas de onda AC simÃ©tricas e centradas em 0 para os 3 canais de tom e ruÃ­do; 2) `s_chip_audio_buf` convertido para `int16_t` signed sem offset residual; 3) Escalamento do DAC 8-bit com centro 128 exato; 4) Recompilado `PicoNGP.uf2` | `PicoNGP.uf2` |
| **PicoNGP (Volume HUD Deslocado)** | O overlay OSD de volume aparecia deslocado para a direita e fora de centro na janela DMG | O buffer `s_slot_data` armazena apenas a janela ativa (`s_active_width`) com inÃ­cio em `s_active_x0` (ex: 51 em Fit, 98 em Integer); o blitter indexava diretamente a coordenada absoluta de tela (`hud_x + i`), somando o offset duas vezes e estourando o buffer em escala 1:1 | Mapeamento da coordenada local `local_x = (hud_x + i) - s_active_x0` com verificaÃ§Ã£o de limites `0 <= local_x < s_active_width` | `PicoNGP.uf2` |
| **PicoGBC / PicoDMG (Performance, Flickering, Combos)** | Queda para 45 FPS e overlay cintilando; toggle de FPS repetindo em loop rÃ¡pido; saÃ­da instantÃ¢nea em SELECT+START | 1) Core 0 bloqueava barramento SPI com `spi_write_blocking` (5.3 KB/frame) concorrendo com bandas DMA do Core 1 que sobrescreviam o HUD; 2) MÃ¡scara `joypad_bits.a = 1` no hold de START forÃ§ava borda de subida contÃ­nua em `prev_joypad_bits.a`; 3) Falta de hold delay em SELECT+START | 1) HUD carimbado scanline-por-scanline no buffer de banda DMA do Core 1 em `mk_ili9225_write_pixels()` (60 FPS contÃ­nuo sem flicker); 2) Desacoplamento com leituras raw de GPIO e flags estÃ¡ticas de borda (`s_last_sta_a`); 3) Hold obrigatÃ³rio de 60 quadros (~1s) para SELECT+START | `PicoGBC.uf2`, `PicoDMG.uf2` |
| **PicoGBC / PicoDMG (Deslocamento Vertical EsporÃ¡dico / SoluÃ§os)** | A imagem inteira se deslocava 1 a 2 pixels para cima muito brevemente (~5 vezes por minuto) | O driver `mk_st7789.c` abria uma janela global Ãºnica de 240 linhas no inÃ­cio do quadro (`st7789_frame_in_progress`) e enviava os blocos DMA sequencialmente sem redefinir coordenadas. Qualquer atraso de fila ou fim de quadro prematuro fazia o ponteiro interno de GRAM do display desalinhar por 1 linha inteira (266 px) | Cada bloco DMA de 24 linhas agora define explicitamente sua prÃ³pria janela de hardware `(x0, y0) .. (x1, y1)` via `set_addr_window_raw` com proteÃ§Ã£o `spi_is_busy` em D/C e CS, travando 100% das scanlines na posiÃ§Ã£o exata | `PicoGBC.uf2`, `PicoDMG.uf2` |
| **PicoGBC / PicoDMG (Tearing em TransiÃ§Ãµes RÃ¡pidas e DegradÃªs)** | Tearing severo cortando a tela horizontalmente no meio de animaÃ§Ãµes/fades no ST7789V / ILI9341 (ex: scroll em Zelda Link's Awakening) | 1) Core 0 emulava em burst (2-3 ms) e enviava scanlines individuais pela FIFO para bandas de 24 linhas; 2) Cada banda reconfigurava janela com `spi_write_blocking` (10x/frame); 3) TransmissÃ£o de ~14,2 ms colidia com varredura padrÃ£o de 60 Hz do ST7789V (~16,6 ms) sem pino TE | 1) Double-buffering nativo completo (160x144 RGB565 em `s_gb_frame_buf[2]`) com despacho atÃ´mico de quadro completo via `CORE_CMD_LCD_FRAME`; 2) Janela de hardware armada uma Ãºnica vez por quadro no Core 1 com ping-pong DMA contÃ­nuo a 72 MHz (lotes de 8 scanlines); 3) RemoÃ§Ã£o da fila de 92 KB `st7789_dma_slots`; 4) Overclock do refresh fÃ­sico do painel ST7789V para 111 Hz via `0xC6` (FRCTRL2) e porches `0xB2`, varrendo a tela em ~9 ms e descolando da escrita SPI | `PicoGBC_hw16.uf2`, `PicoDMG_hw16.uf2`, `PicoGBC.uf2`, `PicoDMG.uf2` |
| **PicoNES (Tearing em Rolagem Vertical / Jogos RÃ¡pidos)** | Leve tearing horizontal em jogos com rolagem vertical/gradientes (ex: Ice Climber, Kid Icarus, Castlevania) | 1) ST7789V varria o display a 60 Hz (~16,6 ms) colidindo com a transmissÃ£o serial SPI a 62 MHz (~17 ms); 2) `ili9341_stream_row` executava 240 vezes por quadro no modo progressivo com ciclos completos de CS/PASET/RAMWR (overhead de 3-4 ms) | 1) Overclock da varredura TFT para 111 Hz (`0xC6=0x01`, `0xB2`) no ST7789V e 100 Hz no ILI9341; 2) Clock SPI elevado para 72 MHz (`SPI_RUN_BAUD`); 3) Janela Ãºnica de hardware configurada uma vez por quadro em `nes_blitter_core1` com CS contÃ­nuo e streaming sem interrupÃ§Ãµes; 4) `set_addr_window` em SRAM (`__not_in_flash_func`) | `PicoNES_hw16.uf2`, `PicoNES.uf2` |
| **Power Switch Standby / Resume (Todos os Emuladores)** | Emuladores travavam em tela preta ou congelavam ao desligar e religar a chave de liga/desliga (`POWER_PIN=32` / `POWER_BEHAVIOR=RESUME`) | 1) `multicore_reset_core1()` deixava o Core 1 congelado em reset e o blitter DMA nunca reiniciava; 2) `xosc_dormant()` destruÃ­a os PLLs e o divisor Flash QMI, travando a CPU ao religar; 3) `start_blitter` nÃ£o existia no contrato `dmg_display_t` | 1) Standby seguro em loop WFI (`sleep_ms(20)` com `watchdog_update()`) preservando PLLs e QMI intactos; 2) Core 1 estacionado de forma cooperativa via `.display.stop_blitter()`; 3) Painel ST7789 colocado em `DISPOFF (0x28)` e `SLPIN (0x10)` com backlight desligado; 4) No wake (ON), reativaÃ§Ã£o limpa via `SLPOUT (0x11)` e `DISPON (0x29)` em 8 bits, restauraÃ§Ã£o do backlight, unpark/rearme de Core 1 via `.display.start_blitter()` e retorno imediato ao loop do jogo | Todos os binÃ¡rios `*_hw16.uf2` recompilados |
| **ProteÃ§Ã£o de GravaÃ§Ã£o Flash XIP & Delay de 1s PÃ³s-Menu** | Chave em OFF durante a carga do emulador na lista para a Flash XIP no bootloader poderia corromper dados ou cortar energia prematuramente | Se a chave for acionada durante `flashAndLaunch()`, o desligamento nÃ£o pode ser imediato; deve concluir a gravaÃ§Ã£o de 100% da Flash e aguardar atÃ© o menu do emulador estar desenhado com 1s de atraso antes de entrar em standby | 1) `pico-bootLoader` ignora a chave durante `flashAndLaunch()` e `handoffToApp()`; 2) `shared_menu` inicia com chave desarmada (`s_power_armed = false`); 3) Ao concluir o desenho do menu (`dmg_rom_selector` e `PicoCC`), chama `dmg_power_arm_menu_loaded(1000)`; 4) Atraso de 1s (1000 ms) decorrido antes de `dmg_power_poll()` aceitar standby | Todos os binÃ¡rios `*_hw16.uf2` e `pico-bootLoader.uf2` recompilados |

---

## 7.1 Fase HW_CONFIG 16 (Waveshare Core2350B) â€” Painel Unificado no `shared_menu`

### Objetivo
A Fase 1 do bootloader passou a escolher o painel em runtime (`ST7789=`, `ROTATE=`, `MADCTL=`, `INVERT=` em `/boot.txt`), mas os 9 emuladores mantinham cada uma sua cÃ³pia do driver com `MADCTL` fixo em `0x28` â€” correto para o ILI9341 do PicoDMG, espelhado no ST7789 do Core2350B. A configuraÃ§Ã£o de painel foi centralizada no `shared_menu` para que os emuladores herdem a escolha do bootloader em vez de repeti-la.

### MÃ³dulo Novo: `shared_menu/dmg_panel.{h,c}`
- `dmg_panel_config_init()`: leitura Ãºnica e cacheada de `/boot.txt` (apÃ³s o SD montado). Tenta `0:/boot.txt`, `/boot.txt`, `boot.txt`, `0:boot.txt`.
- **Parser deliberadamente tolerante**: chaves desconhecidas sÃ£o ignoradas, nÃ£o rejeitadas. O parser estrito e a tela `BOOT.TXT INVALID` sÃ£o do bootloader; um emulador nÃ£o pode ser o motivo de um cartÃ£o bom nÃ£o iniciar, e antes do painel subir nÃ£o hÃ¡ como mostrar diagnÃ³stico Ãºtil.
- API: `dmg_panel_get()`, `get_madctl()`, `get_madctl_derived()`, `has_explicit_madctl()`, `get_rotation()`, `get_invert()`, `get_gram_offset()`, `file_present()`, `source()`.
- `MADCTL=` explÃ­cito tem precedÃªncia sobre `ROTATE` (mesma precedÃªncia do bootloader).
- `DMG_DEFAULT_PANEL_ST7789` (define de build) sÃ³ cobre o caso de cartÃ£o **sem** `/boot.txt`; padrÃ£o `0` preserva todos os builds existentes.

### Tabela de Painel (oriÂ¬ em hardware)
| Painel | Paisagem (`ROTATE=0`) | 180Â° (`ROTATE=1`) | Retrato |
| :--- | :--- | :--- | :--- |
| **ILI9341** (PicoDMG) | `0x28` | `0xE8` | `0x08` |
| **ST7789** (Core2350B) | **`0x60`** | **`0xA0`** | `0x40` |

Os dois eixos foram estabelecidos separadamente para o ST7789:
- **Espelho (`MX`)**: medido em hardware â€” `0x60` = `0x28` + `MX`, mesma orientaÃ§Ã£o paisagem com colunas invertidas. Nenhum datasheet declara os bits de espelho â€” eles dependem de como o controlador Ã© ligado ao vidro, entÃ£o o valor Ã© medido, nÃ£o derivado.
- **Ordem de cor (`BGR`)**: o ST7789 lÃª os dados como RGB com `BGR` **limpo**, o oposto do ILI9341 com o mesmo bit. Herdado em `1`, trocava vermelho e azul (oceano de 1942 saÃ­a vermelho). O bit nunca foi exercido pelo calibrador, porque `0x28` e `0x60` diferem apenas em `MX` â€” todos os 16 candidatos que ele oferecia compartilhavam um Ãºnico BGR. O menu parece correto porque a paleta Clean Light nÃ£o tem vermelho/azul saturados na lista de ROMs.

### Bug Corrigido: 180Â° usava `OR 0xC0` em vez de `XOR 0xC0`
`select_madctl()` (bootloader) e `madctl_for()` (`shared_menu`) usavam `| 0xC0`, produzindo `0xE0` no ST7789 em vez de `0xA0`. Como `0x60` jÃ¡ tem `MX` ligado, `OR` apenas acrescenta `MY` â€” devolvendo o valor invertido do **ILI9341**, ou seja, a mesma imagem espelhada no eixo errado.

O bug Ã© invisÃ­vel no ILI9341, cujo `0x28` nÃ£o tem `MY` nem `MX`, entÃ£o `OR` e `XOR` coincidem; por isso passou pelo build de regressÃ£o e sÃ³ apareceu ao exercitar a rotaÃ§Ã£o no painel novo. `XOR 0xC0` Ã© o correto para os dois painÃ©is e nÃ£o altera nenhum valor do ILI9341.

### PicoNES (1Âº emulador migrado)
- `pico_shared/drivers/ili9341/ili9341.c`: `MADCTL` sai de `dmg_panel_get_madctl()`; sequÃªncia de power-on separada em `st7789_init_sequence()` / `ili9341_init_sequence()`, escolhidas por `dmg_panel_get()`. A sequÃªncia ST7789 Ã© a mesma do `PicoGBC/mk_st7789.c` (`SWRESET(150) â†’ SLPOUT(120) â†’ INVON/INVOFF â†’ COLMOD 0x55 â†’ MADCTL â†’ set_addr_window â†’ NORON(10) â†’ DISPON(120)`), com `INVERT=` do `/boot.txt`.
- `HW_CONFIG 16` portada para `PicoNES/pico_shared/BoardConfigs.cmake`, incluindo I2S MAX98357A em GP26/27/28 (o bloco do bootloader deixa o Ã¡udio desligado, o que quebrava todos os macros `EXT_AUDIO_*` de `external_audio.h`).
- `build_core2350b.ps1` novo; `DEFAULT_PANEL_ST7789=1` explÃ­cito.

### BinÃ¡rios
| Alvo | Arquivo | Tamanho | ValidaÃ§Ã£o |
| :--- | :--- | :--- | :--- |
| **PicoNES HW16** | `PicoNES_hw16.uf2` | 1.160.192 B | [PENDENTE] compila limpo; tabela por painel e defaults ST7789 confirmados por disassembly do ELF |

### Bug: com PSRAM, a ROM nunca chegava a ser carregada (barra de progresso parada)

**Sintoma**: a lista de jogos aparecia normalmente, mas ao escolher um jogo nada acontecia â€” a barra de carregamento nem comeÃ§ava a encher e o jogo nÃ£o iniciava.

**Causa raiz**: `nes_on_load_rom()` (`main.cpp`) sÃ³ tinha o caminho de gravaÃ§Ã£o em flash. Ele deriva o alvo de `ROM_FILE_ADDR - XIP_BASE`, o que sÃ³ faz sentido se `initAll()` tiver atribuÃ­do `ROM_FILE_ADDR`. Essa atribuiÃ§Ã£o (`FrensHelpers.cpp:1897`) estÃ¡ **dentro do ramo "sem PSRAM"**; com a PSRAM presente o `ROM_FILE_ADDR` fica em **0** e o limite de tamanho vira `psramMemorySize` (`:1906`). Logo `flash_offset` virava `0 - 0x10000000` = **`0xF0000000`**, passado para `flashEraseSafe()` â†’ `flash_range_erase()`, que rejeita o offset. A carga morria no primeiro bloco de 16 KB, antes de a barra avanÃ§ar. `flash_rom` tambÃ©m era `nullptr`.

Ou seja: o caminho de flash estava sendo executado numa placa que tem PSRAM, e a placa nunca teve chance de mostrar PSRAM no log.

**CorreÃ§Ã£o**: `nes_load_rom_to_psram()` novo, acionado por `Frens::isPsramEnabled()` **antes** de tocar em `flash_offset`:
- Checagem de tamanho contra `Frens::GetAvailableMemory()` com margem de 512 KB, porque `f_malloc()` entra em `panic()` quando falha.
- `Frens::f_free()` do ROM anterior (flag `s_rom_in_psram`), senÃ£o trocar de jogo vaza cada ROM inteira atÃ© esgotar o alocador.
- `ROM_FILE_ADDR` publicado **antes** da leitura, para que uma carga falha ainda seja reconhecÃ­vel.
- Leitura em blocos de 16 KB, para a barra refletir progresso real e uma leitura curta encerrar o laÃ§o em vez de deixar a cauda do buffer sem inicializar.

**Efeito colateral corrigido**: `getCurrentNVRAMAddr()` (`main.cpp:231`) calcula `ROM_FILE_ADDR - SRAM_SIZE*(slot+1)`, ou seja, a bateria de save Ã© recortada da regiÃ£o da flash logo abaixo da ROM. Com ROM em PSRAM essa subtraÃ§Ã£o aponta para conteÃºdos PSRAM nÃ£o relacionados, que seriam `memcpy`ados como save. Agora retorna 0 nesse caso, deixando o SRAM limpo; os saves continuam indo para o cartÃ£o SD.

| Alvo | Arquivo | Tamanho | ValidaÃ§Ã£o |
| :--- | :--- | :--- | :--- |
| **PicoNES HW16** | `PicoNES_hw16.uf2` | 1.161.728 B | [OK] compila limpo; overclock 111 Hz ST7789V + streaming contÃ­nuo sem tearing |
| **PicoNES HW15** | `PicoNES.uf2` | 1.150.976 B | [OK] compila limpo; overclock 100 Hz ILI9341 + streaming contÃ­nuo |

| **pico-bootLoader HW16** | `pico-bootLoader.uf2` | 564.224 B | [PENDENTE] `XOR` + `BGR` confirmados no ELF (`0x60`/`0xA0` ST7789, `0x28`/`0xE8` ILI9341), mas `ROTATE=1` e `0x60` ainda precisam ser conferidos no painel fÃ­sico |

### PicoPCE (2Âº emulador migrado + suporte a CD-ROM via CHD/PSRAM)
- `pico_shared/drivers/ili9341/ili9341.c`: mesmo padrÃ£o do PicoNES â€” `MADCTL` sai de `dmg_panel_get_madctl()`, power-on separado em `st7789_init_sequence()` / `ili9341_init_sequence()`, escolhidas por `dmg_panel_get()`, com `INVERT=` do `/boot.txt`. FÃ­sica: ST7789 = `0x60` (tabela Â§7.1) via `%%XOR 0xC0`.
- `HW_CONFIG 16` portada para `PicoPCE/pico_shared/BoardConfigs.cmake` (bloco idÃªntico ao do PicoNES, I2S MAX98357A em GP26/27/28).
- `build_core2350b.ps1` novo: `HW_CONFIG 16` + `-DENABLE_CHD=1` + `-DDEFAULT_PANEL_ST7789=1`, saÃ­da `PicoPCE_hw16.uf2` (CHD habilitado); `build_for_bootloader.ps1` continua HW15 com `ENABLE_CHD=0` (sem PSRAM â†’ CD-ROM nÃ£o Ã© listado).

#### CD-ROM (`.cue`/`.chd`) no Core2350B
- **Filtro**: `pce_is_rom_file()` aceita `.chd`/`.cue` apenas quando `#if ENABLE_CHD && Frens::isPsramEnabled()` â€” a HW15 (sem PSRAM) continua sÃ³ `.pce`/`.sgx`.
- **Caminho completo**: o seletor `dmg_rom_selector` devolve nome puro; o ramo `isCDGame` monta o caminho cheio em `s_full_path` a partir de `current_rom_dir` e chama `LoadDisc(s_full_path)` (antes passava o nome relativo, que a `f_open` do CD nÃ£o resolvia).
- **f_free guardado**: o `Frens::f_free(ROM_FILE_ADDR)` do `isCDGame` sÃ³ roda em `#if PICO_RP2350 && PSRAM_CS_PIN && !USE_ILI9341` (mais checagem runtime `>= 0x20000000`), porque no driver case o `ROM_FILE_ADDR` continua em `PCE_ROM_CACHE_ABS` (flash XIP), nÃ£o em ponteiro PSRAM.
- **Bug de CD-DA sem Ã¡udio no case**: `cd_audio_update()` ficava morto no case ILI9341 porque `setVSyncWaitTask` Ã© no-op e o gate `isFrameBufferUsed()` era sempre `false`. O `pushAudioAndOverlay()` agora prefetches CD-DA em `if (CD.audio_status == 0 && (!Frens::isFrameBufferUsed() || USE_ILI9341)) cd_audio_update();` â€” roda no core 0, seguro (FatFS/SD Ã© exclusivo do core 0; `sd_mutex` via `mutex_try_enter` sem bloqueio).
- Todos os buffers de CD (`cd_ram`, `scd_ram`, `adpcm_ram`, `acd_ram`, `bram`, `audio_ring_buf`, files `FIL`) alocam via `frens_f_malloc` â†’ **PSRAM**; o allocator do libchdr Ã© `-include` remapeado por `cd_chd_alloc_wrap.cpp`.
- **DependÃªncia nova**: `external/libchdr` Ã© submodule git â€” clone novo precisa de `git submodule update --init --recursive external/libchdr` antes do build HW16.

| Alvo | Arquivo | Tamanho | ValidaÃ§Ã£o |
| :--- | :--- | :--- | :--- |
| **PicoPCE HW16** | `PicoPCE_hw16.uf2` | 1.132.032 B | [OK] compila limpo; **HuCard `.pce`/`.sgx` agora carrega via `pce_load_rom_to_psram()` na PSRAM** (espelho do `nes_load_rom_to_psram`, flag `s_rom_in_psram`; `ROM_FILE_ADDR` liberado no ramo `isCDGame` antes de `LoadDisc`); **stack do Core 0 relocada de SCRATCH_Y (3 KB) para STACK em RAM (12 KB) via `ld_stack_override/`** (fix de estouro de stack do CHD/CD â€” DiagnÃ³stico 13); validaÃ§Ã£o em hardware pendente (ST7789 sem espelho, `.chd`/`.cue` listados e bootando, CD-DA audÃ­vel) |
| **PicoPCE HW15** | `PicoPCE.uf2` | 643.584 B | [OK] compila limpo; `isPsramEnabled()` falso (PSRAM_CS_PIN=0), caminho de Flash inalterado, seletor `.pce`/`.sgx` inalterado |

> **Requisito de CD-ROM/CHD no Core2350B**: `LoadDisc()` exige um BIOS (`cd_find_bios()` verifica a pasta do `.cue`/`.chd` primeiro, depois `/bios/`). Sem BIOS no cartÃ£o SD, `LoadDisc` retorna `-1` e o jogo nÃ£o abre ("No BIOS in /bios/") â€” Ã© configuraÃ§Ã£o do cartÃ£o, nÃ£o do firmware.

---

## 8. Controle de Volume Global (0..5), Mudo e OSD HUD Compartilhado

### EspecificaÃ§Ã£o de Volume
- **Escala de 6 NÃ­veis**:
  - `0`: Desligado / Ãudio Mudo (`MUTE`). No nÃ­vel 0, o gerador de Ã¡udio e streaming I2S continuam alimentando amostras zeradas para evitar fome de DMA (*DMA starvation* / *underrun*) e manter o sincronismo de clock dos consoles sem emitir som.
  - `1 .. 5`: NÃ­veis progressivos de volume (1 = mÃ­nimo audÃ­vel, 5 = mÃ¡ximo).
- **Menu In-Game (`shared_menu`)**:
  - Exibe `Volume: < Mute >` no nÃ­vel 0, e `Volume: < 1 >` atÃ© `Volume: < 5 >`.
  - Ao ciclar na opÃ§Ã£o com o botÃ£o A, avanÃ§a sequencialmente `(vol + 1) % 6`.
- **PersistÃªncia Global em `0:/boot.txt`**:
  - Salva e lÃª a chave `VOLUME=0..5` (padrÃ£o 3).
  - O `pico-bootLoader` inicializa com esse volume e os emuladores sincronizam a inicializaÃ§Ã£o a partir do `boot.txt`, salvando tambÃ©m no `.cfg` especÃ­fico.

### Atalho de Volume e SupressÃ£o de Conflitos
- **Combo Global**: `SELECT + DPAD UP` (aumenta) e `SELECT + DPAD DOWN` (diminui).
- **SupressÃ£o de AÃ§Ãµes em Jogo**:
  - Enquanto `SELECT` estÃ¡ retido, os cliques de `UP` ou `DOWN` ajustam o volume e tÃªm seus bits suprimidos para que o jogo nÃ£o mova o personagem ou dispare menus in-game.
  - **PicoMD**: GP8 (SELECT) mapeia fisicamente para Genesis C (AÃ§Ã£o/Pulo). Durante o ajuste de volume, GP8 Ã© mascarado para evitar pulos indesejados.
  - **PicoWS**: O toggle de Tate mode (orientaÃ§Ã£o vertical) no release de `SELECT` Ã© bloqueado caso qualquer combo tenha sido acionado durante o hold.
  - **PicoSMS**: `SELECT` atua como Pause no Master System; Ã© suprimido durante a operaÃ§Ã£o de volume.
  - **PicoNES**: Removidos atalhos prÃ©-existentes conflitantes (`SELECT + UP/DOWN` e `START + UP/DOWN`).

### Overlay OSD Visual (HUD)
- **DuraÃ§Ã£o**: Exibido por 1 segundo (1.000 ms) apÃ³s cada ajuste de volume.
- **Geometria e Alinhamento**:
  - **Altura**: 20 pixels (`y = 180 .. 199`, padding inferior de 40px em relaÃ§Ã£o aos 240px de altura).
  - **Largura e PosiÃ§Ã£o X**:
    - Em `GB_MODE=1` (janela DMG 266px): centralizado em `x = 111` (largura 134px, cobrindo `x = 111 .. 244`).
    - Em `GB_MODE=0` (tela cheia 320px): centralizado em `x = 80` (largura 160px, cobrindo `x = 80 .. 239`).
  - **Visual**: Cantos arredondados (raio de 3px), caixa escura contrastante, borda limpa, texto centralizado.
  - **IndicaÃ§Ã£o**:
    - No nÃ­vel `0`: exibe o texto `[ MUTE ]`.
    - Nos nÃ­veis `1 .. 5`: exibe o rÃ³tulo `VOL` seguido por 5 blocos retangulares preenchidos proporcionalmente ao nÃ­vel atual.

### Tabela de BinÃ¡rios Compilados
| Alvo | Arquivo UF2 | Tamanho | ValidaÃ§Ã£o |
| :--- | :--- | :--- | :--- |
| **pico-bootLoader** | `pico-bootLoader.uf2` | 477 KB | [OK] OSD HUD em RAM, eliminaÃ§Ã£o de piscar (single flush), leitura/gravaÃ§Ã£o de `VOLUME` no `boot.txt`, combo `SELECT + UP/DOWN` |
| **PicoA26** | `PicoA26.uf2` | 451 KB | [OK] NÃ­vel 0 mudo, HUD scanline-stamped em `ili9341_stream_row`, zero impacto de FPS |
| **PicoGBC** | `PicoGBC.uf2` | 297 KB | [OK] I2S mapping 0..5, HUD por rajada atÃ´mica SPI 5.3 KB (`mk_ili9225_render_volume_hud`), redraw de borda integer |
| **PicoDMG** | `PicoDMG.uf2` | 297 KB | [OK] I2S mapping 0..5, HUD por rajada atÃ´mica SPI 5.3 KB (`mk_ili9225_render_volume_hud`), redraw de borda integer |
| **PicoLINX** | `PicoLNX.uf2` | 350 KB | [OK] Safe Flash wrappers c/ divisor QMI 4 (90 MHz), watchdog no flash, HUD scanline-stamped em `lynx_band_push` |
| **PicoMD** | `PicoMD.uf2` | 1.69 MB | [OK] Safe Flash wrappers (sem red screen/crash no 2Âº jogo), HUD scanline-stamped em `md_band_push`, C mascarado em combo |
| **PicoNES** | `PicoNES.uf2` | 1.1 MB | [OK] NÃ­vel 0 mudo, HUD scanline-stamped em `nes_blitter_core1`, zero glitches visuais |
| **PicoNGP** | `PicoNGP.uf2` | 1.02 MB | [OK] Safe Flash wrappers, watchdog no flash de jogos grandes, HUD scanline-stamped em `ngp_band_push` |
| **PicoPCE** | `PicoPCE.uf2` | 580 KB | [OK] NÃ­vel 0 mudo, HUD scanline-stamped com dirty hash check, zero tearing/lixo de tela |
| **PicoSMS** | `PicoSMS.uf2` | 842 KB | [OK] NÃ­vel 0 mudo, HUD scanline-stamped com restauraÃ§Ã£o automÃ¡tica em `tmp[]`, redraw de borda integer |
| **PicoWS** | `PicoWS.uf2` | 740 KB | [OK] CorreÃ§Ã£o de ponteiro nulo em I2S audio, Safe Flash wrappers, NEC V30 em SRAM (496 KB), HUD scanline-stamped |


#### ObservaÃ§Ã£o de Cores e Pipeline no `pico-bootLoader`:
- **Formato RGB444 (`0x0RGB`)**: O framebuffer interno do bootloader (`s_fb`) converte pixels para RGB565 dinamicamente no envio SPI via `rgb444_to_rgb565()`. O overlay do HUD de volume no bootloader utiliza as cores correspondentes em RGB444: fundo cinza claro `0x0EEE` (Clean Light UI), texto carvÃ£o `0x0222`, blocos ativos verde menta `0x03C6`, blocos inativos cinza `0x0444` e vermelho puro `0x0F00` no mute (eliminando a distorÃ§Ã£o roxa anterior causada por constantes RGB565 literais).
- **Anti-Flicker (Single Flush)**: O flush prematuro de tela inteira em `gui_draw_frame()` foi transferido para o final do loop principal em `main.cpp`, apÃ³s a composiÃ§Ã£o do HUD de volume sobre o framebuffer em RAM, eliminando o piscar causado pela alternÃ¢ncia de dois flushes consecutivos por quadro.

---

## 9. Arquitetura de RenderizaÃ§Ã£o Scanline sem Impacto de Performance (60 FPS) e CorreÃ§Ãµes de Flash

### 9.1 DiagnÃ³stico do Impacto de Desempenho e Glitches GrÃ¡ficos
Na primeira implementaÃ§Ã£o do HUD de volume, a funÃ§Ã£o `dmg_volume_hud_render()` era executada diretamente no Core 0 dentro do loop de frames do emulador. Essa funÃ§Ã£o desenhava cada pixel ou pequeno bloco atravÃ©s de primitivas grÃ¡ficas (`fill_rect(..., 1, 1, ...)`), gerando **mais de 3.200 comandos SPI avulsos por quadro**.

**ConsequÃªncias CrÃ­ticas:**
1. **Conflito de Barramento SPI (Contention)**: O Core 1 (ou o DMA do blitter de vÃ­deo) estava enviando ativamente linhas do jogo via SPI enquanto o Core 0 tentava assumir o barramento SPI simultaneamente. Isso causava interrupÃ§Ã£o de frames, desincronizaÃ§Ã£o de comandos LCD e artefatos/glitches na tela.
2. **Queda de Framerate (45 FPS ou menos)**: As 3.200 chamadas bloqueavam o Core 0 por dezenas de milissegundos a cada frame, destruindo o sincronismo de 60 FPS da emulaÃ§Ã£o.
3. **Flicker e ResÃ­duos de Tearing**: Ao cessar o HUD apÃ³s 1 segundo, linhas sobrepostas nÃ£o eram redesenhadas pelos emuladores que utilizavam detecÃ§Ã£o de hash/dirty lines, deixando "lixo" grÃ¡fico na tela atÃ© uma atualizaÃ§Ã£o de fundo completa.

### 9.2 SoluÃ§Ã£o: InjeÃ§Ã£o por Linha / Scanline Stamping (Core 1)
Em vez de desenhar sobre a tela a partir do Core 0, o mÃ³dulo compartilhado `shared_menu` passou a disponibilizar uma API leve de consulta em RAM:
- `bool dmg_volume_hud_is_active(void)`: Retorna `true` se o overlay de 1s estiver ativo.
- `const uint16_t *dmg_volume_hud_get_row(uint16_t screen_row, int *out_x, int *out_w)`: Retorna um ponteiro direto para a linha prÃ©-renderizada de pixels RGB565 em RAM (20 linhas x 134/160 pixels) gerada na alteraÃ§Ã£o de volume.

**ImplementaÃ§Ã£o nos Emuladores:**
- **PicoNES, PicoSMS, PicoGG, PicoPCE, PicoA26, PicoMD, PicoNGP, PicoWS, PicoLINX**:
  - Quando a linha a ser transmitida pelo blitter estiver entre `y = 180 .. 199`, o blitter consulta `dmg_volume_hud_get_row(row)` e copia a linha do HUD diretamente para o buffer de transmissÃ£o (`line_packed`, `tmp[]`, `band_buffer`) antes do cÃ¡lculo de hash ou do envio SPI.
  - O Core 0 **nunca toca no display** durante o gameplay.
  - Framerate mantido fixo em **60 FPS sem qualquer atraso**.
  - Glitches grÃ¡ficos, conflitos de barramento e flickering foram **100% eliminados**.
  - Como a injeÃ§Ã£o ocorre no pipeline normal de vÃ­deo antes da comparaÃ§Ã£o de hash (ex: SMS, GG, PCE), a tela se recupera e limpa instantaneamente assim que o HUD expira.
- **PicoGBC / PicoDMG**:
  - Implementada a funÃ§Ã£o de envio atÃ´mico em bloco `mk_ili9225_render_volume_hud()`. Quando o HUD estÃ¡ ativo, as 20 linhas (5.3 KB de pixels prÃ©-compostos) sÃ£o enviadas em uma Ãºnica transferÃªncia contÃ­gua `spi_write_blocking` apÃ³s o tÃ©rmino do frame, reduzindo o overhead de SPI de milhares de comandos para apenas 1 bloco rÃ¡pido.
- **pico-bootLoader**:
  - EliminaÃ§Ã£o do piscar (*anti-flicker*): O HUD Ã© desenhado diretamente no framebuffer RGB444 em RAM antes do flush de vÃ­deo, eliminando o duplo flush consecutivo.

### 9.3 DiagnÃ³stico e CorreÃ§Ã£o de Falhas de Carregamento de Flash (PicoWS, PicoMD, PicoNGP, PicoLINX)
Ao selecionar ou alternar jogos nesses emuladores, ocorriam travamentos na tela de carregamento, telas pretas, retorno ao menu ou a clÃ¡ssica tela vermelha (*red screen*):

1. **Overclock da Flash XIP (180 MHz vs 90 MHz)**:
   - A CPU RP2350 roda com clock de 360 MHz (`clk_sys`).
   - O controlador de memÃ³ria Flash XIP possui um divisor configurÃ¡vel (`QMI_M0_TIMING_CLKDIV`). Para operaÃ§Ã£o estÃ¡vel a 360 MHz, esse divisor deve ser no mÃ­nimo **4** (gerando 90 MHz de clock SPI de Flash).
   - As rotinas de baixo nÃ­vel da Bootrom (`flash_range_erase` e `flash_range_program`), ao concluÃ­rem a gravaÃ§Ã£o, chamam `boot2` que reseta o divisor QMI para **2** (180 MHz).
   - Ao religar as interrupÃ§Ãµes e voltar a executar cÃ³digo em Flash com divisor 2 a 360 MHz, a CPU sofria corrupÃ§Ã£o imediata de memÃ³ria e `HardFault`, causando reboot ou aborto do carregamento.
   - **CorreÃ§Ã£o**: ImplementaÃ§Ã£o dos wrappers seguros `safe_flash_range_erase()` e `safe_flash_range_program()` alocados em RAM (`__no_inline_not_in_flash_func`), que garantem a chamada de `apply_safe_qmi_timing()` (fixando o divisor QMI em 4) **antes** de habilitar as interrupÃ§Ãµes e retornar Ã  Flash.
2. **Watchdog Timeout em ROMs Grandes**:
   - Em ROMs maiores (jogos de 2MB a 4MB), o processo de apagamento e escrita em Flash excedia a janela do hardware watchdog (6 segundos).
   - **CorreÃ§Ã£o**: InserÃ§Ã£o de chamadas `watchdog_update()` dentro dos loops de programaÃ§Ã£o em bloco por bloco de Flash.




---

## 24. Recursos Extras de Cartucho: RTC (MBC3) e Rumble (MBC5) (01/10/2026)

### 24.1 CorreÃ§Ã£o CrÃ­tica: Overflow da SRAM do Cartucho

**Sintoma latente**: um cartucho declarando  x0149 = 0x04 (128 KB) endereÃ§ava 128 KB dentro de um buffer de 32 KB. Confirmado por 
m no ELF:

`
2002a6c0 00008000 b ram        <- 32 KB
200326c0 00000004 b read_index
200326c4 00010000 b rom_bank0  <- cache de ROM de 64 KB, VIVA
`

O que vem logo depois de 
am Ã© 
om_bank0, a janela de cache do ROM ativo. Overflow escrevia **dentro do cÃ³digo do jogo emulado**, nÃ£o num framebuffer.

**Correcoes**:
- 
am[32768] -> 
am[65536] (main.c). 64 KB cobre  x0149 = 0x05, o maior valor real. 128 KB consumiria o heap livre (~171 KB) e quebraria o allocator lwmem do HW16. Valor vindo do CMake (GB_CART_RAM_MAX_SIZE) para nao divergir do core.
- gb_cart_ram_read/write: clamp com sizeof(ram); leitura fora retorna  xFF (igual ao hardware).
- 
ead_cart_ram_file: _read(..., f_size(&fil)) -> _read(..., save_size). Um .sav truncado ou hostil era um segundo caminho de overflow.
- peanut_gb.h: 
um_ram_banks com clamp no core.
- gb_get_save_size(): 
am_sizes[] tinha 5 entradas para 6 codigos de header; header  x05 lia fora do array. Corrigido.

### 24.2 RTC do MBC3 (Pokemon Gold, Silver, Crystal)

O core ja tinha os registradores (cart_rtc[5]), e gb_tick_rtc()/gb_set_rtc() (do SameBoy) nunca eram chamadas. O relogio ficava eternamente em 00:00:00, dia 0.

**Nao existe hardware de relogio**: o periferico RTC existe no RP2040 (hardware/regs/rtc.h) mas **nao no RP2350**, que o removeu. 	ime_init() e chamado sem fonte. Solucao 100% software.

- cart_rtc_latched[5]: registradores MBC3 tem dois conjuntos; leituras saem do conjunto travado, nao do relogio rodando.
- LATCH_CLOCK (bit 6 do P1, borda 1->0) copia rodando -> travado.
- gb_tick_rtc_subsecond(): gb_tick_rtc() por quadro rodaria o relogio 59,73x rapido. O acumulador de 1/64 s usa o comprimento real do quadro (70224 ciclos, 140448 em double speed lido do KEY1).
- Persistencia: 10 bytes (5 + 5) anexados ao .sav, lidos de volta **somente** se _size() == save_size + 10. Saves Gold/Silver antigos continuam validos.
- has_rtc vem do **tipo do cartucho** ( x0F/ x10), nao de mbc == 3:  x12 (MBC3+RAM) tem RAM mas nao tem timer.

### 24.3 Rumble do MBC5 (Mario Tennis, Pokemon Pinball, Perfect Dark)

Tabela canonica de tipos de cartucho:

| Tipo | Descricao |
| :--- | :--- |
|  x1C | MBC5 + RUMBLE |
|  x1D | MBC5 + RUMBLE + RAM |
|  x1E | MBC5 + RUMBLE + RAM + BATTERY |

O motor fica no **bit 3 do RAM bank register** ( x4000-0x5FFF). Codigo anterior fazia al & 0x07 e descartava o bit.

- irmware/gb_rumble.c: driver novo, gpio_put puro (rumble de MBC5 e on/off, PWM seria emblema em vez de emulacao). Pino vem de RUMBLE= no /boot.txt; ausente/0 = motor desligado. GPIOs 0..9 recusados (sao os botoes). Motor via **transistor** (2N2222), nunca direto no GPIO.
- gb_rumble_shutdown() no out:, e gb_rumble_set(false) no Reset do menu, para o motor nao ficar ligado na saida.

### 24.4 Dois Bugs de Rumble Corrigidos em Hardware

**1. Menu do Pokemon Pinball travado (A/B/SELECT mortos)** â€” este foi um bug introduzido na primeira implementacao, nao pre-existente.

Tinha-se escrito um readback no P1 ( xFF00) colocando o estado do motor nos bits 0-2. O Pandocs e o SameBoy mostram que **isso nao existe**: num MBC5 com rumble o P1 e um P1 normal, o motor fica so no bit 3 do bank register e o P1 nunca e tocado. O codigo fazia:

`c
gb->hram_io[IO_JOYP] = (gb->hram_io[IO_JOYP] & 0xF8) | (rumble ? 0x00 : 0x07);
`

O & 0xF8 **apagava os bits 0-2, que sao A, B e SELECT**. Como o hack so rodava com has_rumble, **so cartuchos de rumble quebravam** â€” os outros jogos seguiam normais, o que confirmou o diagnostico. Removido das duas vias (escrita e leitura).

**2. Mario Tennis sem rumble** â€” a deteccao listava so  x1D e  x1E, faltando o  x1C. Mario Tennis e  x1C, Pokemon Pinball e  x1D. Corrigido para os tres.

### 24.5 Chaves RUMBLE= e RTC_EPOCH= no boot.txt

O pico-bootLoader usa parser de **whitelist** (sd_boot_ini.c): chave desconhecida **rejeita o arquivo inteiro**, zerando GB_MODE, VOLUME, painel e rotacao, e levantando BOOT.TXT INVALID. As duas chaves precisam ser registradas la, nao so parseadas no emulador.

- sd_boot_ini.h/.c: campos 
umble_pin e 
tc_epoch[24], flags seen_*, e a lista "valid:" da mensagem de erro.
- shared_menu/dmg_panel.c: parse das mesmas chaves, na leitura unica e cacheada.
- Escritor nao muda: so GUI= e THEME= sao reescritos, o resto passa byte a byte.
- RTC_EPOCH e lido **uma unica vez**, no primeiro boot. O relogio depois vive no .sav e so conta pra frente.

**Bug pre-existente descoberto**: dmg_panel.c nao estava na lista de fontes do PicoGBC (src/CMakeLists.txt), so dmg_menu.c. Nenhum simbolo dmg_panel_get_* linkava, apesar de declarados em dmg_menu.h. Adicionado.

### 24.6 Bug de Build: build_hw16 sobrescrevia o binario HW15

O post-build do CMake copiava para a raiz do projeto com nome fixo (PicoGBC.uf2) nas duas configuracoes, entao uild_hw16 **sobrescrevia o binario ILI9341** pelo ST7789, mesmo nome, errado para um console PicoDMG. A copia para a raiz agora e condicionada a HW_CONFIG != 16; o HW16 usa o nome com sufixo via uild_core2350b.ps1. Isso causou confusao em teste (pegar a build errada e achar que o display tinha mudado).

### 24.7 Estado de Validacao

| Teste | Status |
| :--- | :--- |
| Rumble no Pokemon Pinball ( x1D) | [OK] motor vibra, menu options responde (confirmado pelo usuario) |
| Rumble no Mario Tennis ( x1C) | [OK] motor vibra (confirmado pelo usuario) |
| RTC: relogio avanca, bate com a hora real, sobrevive a sair/voltar | [PENDENTE] |
| .sav Gold/Silver antigo ainda carrega | [PENDENTE] |
| Regressao MBC1 + MBC3 + MBC5 comuns apos o resize do 
am[] | [PENDENTE] |

**Ressalva**: struct gb_s cresceu, entao savestates (.ss) antigos passam a ser rejeitados. O loader compara sizeof(struct gb_s) e avisa em vez de ler errado, mas os states precisam ser refeitos.
---

## 25. Camada Comum de Combos In-Game (`shared_menu/dmg_combo.c`)

### O Problema

Cada um dos 9 emuladores tinha a sua propria copia da logica de combos em
`main.cpp` (ou `main.c` no PicoGBC). As copias tinham derivado:

| Divergencia | Detalhe |
| :--- | :--- |
| 4 tipos diferentes de `s_exit_hold_frames` | `int` x3, `uint16_t` x1 |
| 2 politicas de reset do contador | o PicoWS nao zerava ao disparar |
| Bloco **duplicado** no PicoPCE | linhas 1347 e 1468, com predicados de borda diferentes |
| Predicado de borda errado no PicoPCE | `(!prev_a \|\| !prev_start)` dispara tambem quando **so um** dos dois foi novo |
| 6 emuladores **sem SELECT+A** | so PicoGBC, PicoNGP e PicoWS tinham o toggle 1x/2x |
| Mascara de botoes inoperante no PicoPCE | ver bug corrigido abaixo |
| Sem deteccao de borda no PicoA26 e no PicoNES | segurar o combo re-dispara a cada quadro |

O `shared_menu/dmg_menu.c` so tinha `dmg_process_volume_combo()`. SELECT+START,
SELECT+A, START+A e START+B nao existiam no modulo comum.

### O Modulo

`shared_menu/dmg_combo.h` + `shared_menu/dmg_combo.c`.

API publica:

```c
typedef struct {
    bool up, down, left, right, a, b, select, start;
} dmg_pad_t;                      /* todos os campos: true = pressionado */

typedef struct {
    bool request_pause_menu;       /* START + B */
    bool request_exit_game;        /* SELECT + START retido */
    bool speed_toggled;            /* SELECT + A (1x <-> 2x) */
    bool fps_toggled;              /* START + A */
} dmg_combo_events_t;

void dmg_combo_reset(void);
void dmg_combo_frame(dmg_pad_t *pad, const dmg_emulator_desc_t *desc,
                     dmg_combo_events_t *ev);
bool dmg_combo_volume_active(void);
```

`pad` e **lido e escrito**: todo botao consumido por um combo e zerado antes de
retornar. E o proprio mascaramento que impede o combo de chegar ao console
emulado como uma pressao espuria -- o chamador so repassa o pad.

### Prioridade

Maior primeiro, quando varios combos casam na mesma pressao:

1. **SELECT + START** -- sair do jogo vence sobre tudo
2. **START + B** -- menu de pausa (nao pode ser engolido por FPS/velocidade)
3. **START + A** -- overlay de FPS
4. **SELECT + A** -- 1x / 2x
5. **SELECT + UP/DOWN** -- volume (0..5)

### Edge vs Hold

Um toggle nao pode re-disparar enquanto o botao esta retido, entao os toggles
(START+A, SELECT+A) sao **edge triggered**. A saida de jogo e o unico combo que
destroi estado, entao e **hold**: `DMG_COMBO_EXIT_HOLD_FRAMES = 60` (~1 s a 60 Hz),
o valor ja validado no hardware do PicoGBC/PicoDMG, PicoNGP e PicoWS.

### SELECT+A escreve o mesmo estado que o menu

O toggle faz exatamente o que a linha `Speed` do menu in-game faz
(`dmg_menu.c`, cursor 2):

```c
g_dmg_config.turbo_mode = !g_dmg_config.turbo_mode;
desc->apply_speed(g_dmg_config.turbo_mode != 0);
desc->save_config();
```

Assim o combo e o menu nunca discordam sobre a velocidade atual.

### 3 callbacks novos no descritor

```c
void (*save_config)(void);   /* "grava minha config agora" */
bool (*get_fps)(void);       /* overlay de FPS ligado? */
void (*apply_fps)(bool on);  /* alterna o overlay */
```

`save_config` em vez de `dmg_config_save()` direto porque cada emulador grava do
seu jeito: o PicoNES usa uma struct privada (tem `crop_sides` e `height_fill` que
nao cabem em `dmg_config_t`, entao o escritor compartilhado perderia os dois
silenciosamente) e o PicoWS precisa sincronizar os proprios statics em
`g_dmg_config` antes de gravar. Passar por um unico callback e o que impede cada
combo de perder um desses passos.

### Bug Corrigido: as mascaras do PicoPCE nao faziam nada

O bloco de combos do PicoPCE rodava **depois** de
`PCE.Joypad.regs[i] = pcebuttons | pcesystem[i];`. Ele limpava bits das copias
locais `pcebuttons` / `pcesystem` -- mas o registro que o jogo le ja tinha sido
atribuido. O resultado era que os botoes de combo **vazavam para o PC Engine em
todos os quadros em que ficavam retidos**, e a mascara nao evitava nada.

Corrigido movendo a passada de combos para **antes** da escrita do registro, junto
com o restante dos emuladores.

### Bug Corrigido: PicoGBC perdia o START+A e o SELECT+START

O PicoGBC mantinha `a = 1; b = 1;` (released = 1, convencao ativa-baixa do core) e
usava `raw_*` invertido para detectar combos. Ao migrar, o pad e entregue na
convenÃ§Ã£o de "pressionado" e reconvertido depois, de modo que o modulo comum vÃª a
mesma polaridade nos 9. O `raw_select` remanescente e o que distingue "o jogador
esta segurando SELECT" de "SELECT foi consumido por um combo", e e o que impede
os atalhos de paleta (`SELECT+LEFT` / `SELECT+RIGHT`) de dispararem em
`SELECT+A`.

### Alteracoes Comportamentais (decididas pelo usuario)

| Emulador | Antes | Agora |
| :--- | :--- | :--- |
| **PicoSMS / PicoGG** | SELECT+START instantaneo | hold de 60 frames; **SELECT+A = 1x/2x** (novo) |
| **PicoA26** | SELECT+START instantaneo | hold de 60 frames; **SELECT+A = 1x/2x** (novo) |
| **PicoLNX** | SELECT+START por borda | hold de 60 frames (ja era 60, agora compartilhado) |
| **PicoNES** | SELECT+START instantaneo; SELECT+A = *rapid fire* no A | hold de 60 frames; SELECT+A = **1x/2x** |
| **PicoMD** | ja tinha hold de 60 | inalterado; **SELECT+A = 1x/2x** (novo) |
| **PicoPCE** | SELECT+START por borda (predicado errado) | hold de 60 frames; mascaras agora efetivas; **SELECT+A = 1x/2x** (novo) |
| **PicoNGP / PicoWS / PicoGBC / PicoDMG** | ja tinham hold de 60 | comportamento inalterado, agora via modulo comum |

**PicoNES, rapid fire**: `SELECT+A` alternava rapid fire no botao A
(`rapidFireMask[i] ^= A`). Isso colidia com o novo toggle de velocidade, e o
usuario escolheu trocar. O rapid fire no **B** (`SELECT+B`) foi mantido.

### Chamada por emulador

```c
dmg_pad_t pad_in;
read_ui_buttons(&pad_in.up, &pad_in.down, ... &pad_in.select, &pad_in.start);

dmg_combo_events_t ev;
dmg_combo_frame(&pad_in, &s_xxx_desc, &ev);

if (ev.request_pause_menu) s_request_pause_menu = true;
if (ev.request_exit_game)  s_request_exit_game  = true;
/* ... repassar pad_in (ja mascarado) ao console emulado ... */
```

`dmg_combo_reset()` e chamado no inicio de cada sessao de jogo. O estado e
file-static, entao sem isso um hold de SELECT+START pela metade do jogo anterior
seria herdado: soltar e apertar de novo um frame apos carregar um jogo sairia
imediatamente, o que parece exatamente o hold sendo ignorado.

### Casos que precisam de cuidado na migracao

- **PicoWS** (rotacao Tate no release do SELECT): o toggle precisa do SELECT
  **cru**. Ler o pad ja mascarado faria `SELECT+A` girar a tela quando SELECT
  solta, entao o codigo copia `raw_*` antes de `dmg_combo_frame()`.
- **PicoMD** (`s_supress_input`): e um latch, nao "um combo disparou neste
  quadro". Precisa continuar travado ate todos os botoes serem soltos, porque o
  jogador ainda esta segurando o combo nos quadros seguintes a borda.
- **PicoPCE**: `BOOTSEL` e um acorde de 4 botoes em cima de SELECT+START, e no
  PicoNES tambem. Precisa ser testado **antes** da camada de combos, senao
  SELECT+START e tomado pela saida de jogo.
- **PicoGBC**: `START+B` fica **fora** do modulo, porque o menu e sincrono (estaciona
  o core do LCD, roda o menu, volta) e nao cabe num flag de retorno. O resto da
  deteccao vai pelo modulo comum.

### Binarios

Todos os 9 emuladores, nas duas placas. Compilam limpo, sem warnings novos.

| Alvo | HW15 (RP2350 Plus / ILI9341) | HW16 (Core2350B / ST7789) |
| :--- | --- | --- |
| **PicoA26** | `PicoA26.uf2` 486.912 B | `PicoA26_hw16.uf2` 497.664 B |
| **PicoGBC** | `PicoGBC.uf2` 426.496 B | `PicoGBC_hw16.uf2` 432.640 B |
| **PicoDMG** | `PicoDMG.uf2` 426.496 B | `PicoDMG_hw16.uf2` 432.640 B |
| **PicoLNX** | `PicoLNX.uf2` 425.472 B | `PicoLNX_hw16.uf2` 428.032 B |
| **PicoMD** | `PicoMD.uf2` 1.839.616 B | `PicoMD_hw16.uf2` 1.849.856 B |
| **PicoNES** | `PicoNES.uf2` 1.160.704 B | `PicoNES_hw16.uf2` 1.171.456 B |
| **PicoNGP** | `PicoNGP.uf2` 1.026.048 B | `PicoNGP_hw16.uf2` 1.036.800 B |
| **PicoPCE** | `PicoPCE.uf2` 653.312 B | `PicoPCE_hw16.uf2` 1.142.272 B |
| **PicoSMS** | `PicoSMS.uf2` 870.400 B | `PicoSMS_hw16.uf2` 882.176 B |
| **PicoGG** | `PicoGG.uf2` 869.888 B | `PicoGG_hw16.uf2` 881.664 B |
| **PicoWS** | `PicoWS.uf2` 746.496 B | `PicoWS_hw16.uf2` 758.784 B |

Verificado por `nm` em todos os ELFs: `dmg_combo_frame` e `dmg_combo_reset`
linkados. Os 4 emuladores sem FSM proprio (SMS, A26, LINX, NGP) confirmam que
`dmg_combo.c` substituiu de fato as copias locais -- nao sobrou nenhum
`s_exit_hold_frames`, `combo_exit`, `last_sta_*` ou `prev_combo_*` nos 9
`main.cpp` / `main.c`.

### Pendente de Validacao em Hardware

Compilacao limpa nao prova que a migracao esta correta em hardware. O que precisa
ser testado no aparelho:

1. **SELECT+A alterna 1x/2x** nos 6 emuladores onde e novo (SMS/GG, A26, LNX, NES, MD, PCE), e continua funcionando nos 3 que ja tinham (NGP, WS, GBC/DMG).
2. **SELECT+START so sai depois de ~1 s** -- e nao antes. particularly no PicoSMS/PicoGG, onde SELECT e Pause no Master System: segurar os dois por 1 s nao pode dar 60 Quadros de Pause engolido pelo jogo.
3. **O console emulado nao ve os botoes de combo.** particularly no PicoPCE, cujo bug de mascaras so agora foi corrigido de verdade, e no PicoMD, onde GP8 (SELECT) e o botao C da Genesis -- `SELECT+A` nao pode virar um pulo em Sonic 2.
4. **PicoWS**: `SELECT+A` nao deve girar a tela (Tate) ao soltar SELECT.
5. **PicoNES**: `SELECT+START+UP+A` ainda vai para BOOTSEL (testado antes da camada de combos).
6. **PicoGBC**: `SELECT+LEFT` / `SELECT+RIGHT` ainda mudam a paleta, e `START+A` ainda liga o overlay de FPS.
7. **Nenhum hardware travou ou reiniciou** ao segurar dois botoes durante o gameplay.

---

## 26. Timing Fisico do Painel ST7789 Centralizado (`dmg_panel_st7789_timing`)

### Contexto

O overclock de 111 Hz foi introduzido para corrigir **tearing vertical**, que era grande. O
mecanismo: o painel varre o vidro a uma taxa fixa, que o emulador nao controla. No
console, o blitter do Core 1 leva ~14 ms a 72 MHz de SPI para escrever um quadro, enquanto
a varredura de fabrica do ST7789 leva ~16,6 ms (60 Hz). Sao a mesma ordem de grandeza, entao
a escrita na GRAM e a varredura do feixe se sobrepoem -- e a sobreposicao aparece como um
corte horizontal atravessando fades, cutscenes e rolagem de tela cheia.

Overclockando a varredura para ~111 Hz (~9 ms), a varredura termina muito antes de o proximo
quadro ser escrito.

| Registrador | Valor | Funcao |
| :--- | :--- | :--- |
| `0xB2` PORCTRL | `0x0C, 0x0C, 0x00, 0x33, 0x33` | porches, back porch apertado |
| `0xC6` FRCTRL2 | `0x01` | 111 Hz (`0x0F` e o default de 60 Hz) |

### Bug: no PicoNES a configuracao estava na funcao errada

A alteracao de 30/09 foi aplicada no PicoNES, e de fato chegou ao binario -- mas caiu dentro
de `ili9341_init_sequence()`, e nao de `st7789_init_sequence()`.

```
PicoNES/pico_shared/drivers/ili9341/ili9341.c
  L302  st7789_init_sequence()    <- chamada em L360 quando o painel e ST7789. SEM timing.
  L385  ili9341_init_sequence()   <- chamada em L362 quando o painel e ILI9341.
         L410  0xB1  FRMCTR1     (ILI9341-only, correto aqui)
         L416  0xB2  PORCTRL     (ST7789-only, NAO deveria estar aqui)
         L421  0xC6  FRCTRL2     (ST7789-only, NAO deveria estar aqui)
```

Consequencias, nenhuma delas visivel no boot:

- **Core2350B (ST7789):** a sequencia do ST7789 nao tinha timing, entao o console rodava nos
  60 Hz de fabrica. **O overclock nunca teve efeito no PicoNES.** Era exatamente o tearing que
  o overclock existe para corrigir.
- **RP2350 Plus (ILI9341):** recebia `0xB2` e `0xC6`, que nao existem no conjunto de comandos
  do ILI9341. Sem operacao, sem erro, sem mudanca visivel.

O firmware sobe identico nas duas placas, e por isso passou: um driver que simplesmente
esquece o overclock nao tem sintoma de boot, so tem tearing depois. Este e o mesmo modo de
falha que motivou Â§25 -- a divergencia entre copias, invisivel ate alguÃ©m testar.

### Centralizacao

`shared_menu/dmg_panel.h` / `dmg_panel.c`:

```c
typedef void (*dmg_panel_write_fn)(uint8_t cmd, const uint8_t *data, size_t len);
void dmg_panel_st7789_timing(dmg_panel_write_fn write);
```

Um unico callback combinado (comando + bloco de parametros) em vez de `write_cmd` +
`write_data`: e a assinatura que o `st7789_write_command_data()` do PicoGBC ja tem, entao o
PicoGBC passa a funcao direto. Os oito drivers ILI9341, que mantem as duas primitivas
separadas, passam um adaptador de quatro linhas:

```c
static void panel_timing_write(uint8_t cmd, const uint8_t *data, size_t len) {
    write_cmd(cmd);
    if (data != NULL && len > 0) write_data_buf(data, len);
}
```

Chamada, sempre dentro de `st7789_init_sequence()`, depois do MADCTL e antes do
`set_addr_window()`:

```c
dmg_panel_st7789_timing(panel_timing_write);
```

### Estado por emulador

| Emulador | Antes | Agora |
| :--- | :--- | :--- |
| **PicoGBC / PicoDMG** | bloco proprio, valores corretos | mesmo bloco, via helper |
| **PicoNGP** | bloco proprio, valores corretos | via helper |
| **PicoWS** | bloco proprio, valores corretos | via helper |
| **PicoNES** | **na branch do ILI9341** | **corrigido para a branch do ST7789** |
| **PicoPCE** | ausente (60 Hz) | via helper |
| **PicoMD** | ausente (60 Hz) | via helper |
| **PicoSMS / PicoGG** | ausente (60 Hz) | via helper |
| **PicoLINX** | ausente (60 Hz) | via helper |
| **PicoA26** | ausente (60 Hz) | via helper |

`pico-bootLoader` fica de fora: e so ILI9341 e nao linka `shared_menu`.

Verificacao: nenhum literal `0xB2`/`0xC6` cru sobrou em nenhum driver; `nm` confirma
`dmg_panel_st7789_timing` linkado nos 9 ELFs; o disassembly do PicoGBC mostra
`movs r0, #178` (0xB2) com len 5 e `movs r0, #198` (0xC6) com len 1 dentro da funcao
compartilhada; e no PicoNES a unica chamada a ela esta entre `dmg_panel_get_madctl()` e
`set_addr_window()` -- a posicao da sequencia do ST7789.

### Direcao obrigatoria dos registradores

Os dois erros sao simetricos e opostos, e por isso nenhum dos dois e visivel:

| Registrador | Painel | Consequencia se enviado ao painel errado |
| :--- | :--- | :--- |
| `0xB1` FRMCTR1 | **ILI9341-only** | nada no ST7789 (comando nao atribuido) |
| `0xB2` PORCTRL | **ST7789-only** | nada no ILI9341 |
| `0xC6` FRCTRL2 | **ST7789-only** | nada no ILI9341 |

Um painel ignora o que nao conhece, entao mandar oTiming errado **nunca** produz erro. So
produz a ausencia do efeito. E por isso que a centralizacao vale mais aqui do que em outros
lugares: o valor esta num lugar, e o lugar certo e o unico que o chamador pode errar
silenciosamente.

### Pendente: `FRMCTR1` do ILI9341 (`0xB1`) continua divergente

Nao centralizado, porque e um caso diferente e a decisao exige medicao em hardware. O valor
esta em **dois lugares** no projeto:

| Valor | Sites | Comentario no codigo |
| :--- | :--- | :--- |
| `0x00, 0x18` | A26, LINX, MD, NGP, PCE, SMS, WS, `PicoGBC/mk_ili9341.c`, `pico-bootLoader` | "~79 Hz" |
| `0x00, 0x10` | `PicoNES`, `PicoGBC/mk_st7789.c` | "~100 Hz" |

Os dois comentario sao estimativas: ninguem mediu a varredura real do ILI9341. `0x18` e o
valor classico de tabela (TFT_eSPI, Adafruit) e o que oito emuladores usam hoje no RP2350
Plus, portanto e o caminho efetivamente validado nesta placa.

Dois erros de documentacao encontrados junto:

1. O `AGENTS.md` do PicoNES diz que o FRMCTR1 foi posto em **`0x00, 0x1B`** para 100 Hz. O
   codigo tem **`0x00, 0x10`**. Nem a doc nem o codigo dizem 0x1B em lugar nenhum do fonte.
2. O `AGENTS.md` raiz afirma, na tabela de tearing, que o ILI9341 foi a 100 Hz via
   `0x00, 0x1B`. Isso so e verdade para o PicoNES; os outros oito estao em `0x18`.

**Nao foi alterado.** Uniformizar mudaria o comportamento de oito emuladores no HW15 com base
em um valor nao medido. Se o objetivo for o mesmo do ST7789 -- desacoplar a varredura da
escrita SPI no RP2350 Plus -- o caminho e medir primeiro o que `0x18` e `0x10` dao no vidro
do PicoDMG, e so entao escolher. A centralizacao em `dmg_panel.c` (como
`dmg_panel_ili9341_timing()`) pode ser feita em qualquer momento; o que nao pode ser feito e
escolher o numero sem medicao.

### Binarios

18 UF2 recompilados (9 emuladores x HW15 + HW16), todos limpos.

| Alvo | HW15 | HW16 |
| :--- | --- | --- |
| **PicoA26** | `PicoA26.uf2` 487.424 B | `PicoA26_hw16.uf2` 498.176 B |
| **PicoGBC** | `PicoGBC.uf2` 426.496 B | `PicoGBC_hw16.uf2` 432.128 B |
| **PicoDMG** | `PicoDMG.uf2` 426.496 B | `PicoDMG_hw16.uf2` 432.128 B |
| **PicoLNX** | `PicoLNX.uf2` 425.984 B | `PicoLNX_hw16.uf2` 428.544 B |
| **PicoMD** | `PicoMD.uf2` 1.840.128 B | `PicoMD_hw16.uf2` 1.850.368 B |
| **PicoNES** | `PicoNES.uf2` 1.160.704 B | `PicoNES_hw16.uf2` 1.171.456 B |
| **PicoNGP** | `PicoNGP.uf2` 1.026.048 B | `PicoNGP_hw16.uf2` 1.036.288 B |
| **PicoPCE** | `PicoPCE.uf2` 654.336 B | `PicoPCE_hw16.uf2` 1.143.296 B |
| **PicoSMS** | `PicoSMS.uf2` 870.912 B | `PicoSMS_hw16.uf2` 882.688 B |
| **PicoGG** | `PicoGG.uf2` 870.400 B | `PicoGG_hw16.uf2` 882.176 B |
| **PicoWS** | `PicoWS.uf2` 745.984 B | `PicoWS_hw16.uf2` 758.272 B |

### Pendente de Validacao em Hardware

O unico teste que interessa, e so no Core2350B:

1. **Tearing vertical no Core2350B**, agora nos 9. Os 5 que estavam sem o overclock (PCE, MD, SMS/GG, LINX, A26) sao o teste principal -- antes deles o console rodava a 60 Hz de fabrica e a diferenca deve ser visivel em qualquer fade ou rolagem de tela cheia.
2. **PicoNES no Core2350B**: e a correcao de fato. Se o tearing vertical persistir, o bloco nao foi aplicado onde pensavamos.
3. **Nada regrediu no HW15**: os 8 que gainham `0xB2`/`0xC6` agora param de envia-los ao ILI9341, o que era uma operacao sem efeito -- nao deve haver mudanca visivel, e convem confirmar.
4. **PicoGBC/PicoDMG sem mudanca**: seus valores ja eram os mesmos, so a origem mudou.

---

## 27. Persistencia das Configuracoes â€” tres bugs independentes

### Sintoma

"Altero as opcoes no menu in-game e nao persiste. Sempre que volto ao jogo, as configuracoes
da rodada anterior nao ficam."

Verificado em **9 emuladores**, nao em um. E nao era um bug so: eram tres, independentes,
cada um com um sintoma proprio. Dois deles so apareceram quando os arquivos de `I:\saves`
foram lidos no PC em vez de deduzidos do codigo.

### Bug 1 â€” O menu gravava a copia errada do estado (6 emuladores)

O emulador guarda as opcoes em `persistent_*`, e o `dmg_config_save()` escreve
`g_dmg_config`. Sao **duas copias do mesmo estado**.

O menu in-game **compartilhado** (`dmg_menu.c`) escreve `g_dmg_config` primeiro e deixa o
callback do descritor empurrar para baixo:

```c
g_dmg_config.turbo_mode = !g_dmg_config.turbo_mode;
desc->apply_speed(...);          /* -> persistent_turbo_mode */
dmg_config_save(desc);           /* -> escreve g_dmg_config, que esta correto */
```

O submenu **customizado** de cada emulador faz o contrario -- muda `persistent_*` e chama o
save, que escreve `g_dmg_config` **sem ter sido tocado**:

```c
persistent_scale_mode = ...;      /* so a copia local mudou */
ili9341_set_scale_mode(...);
save_picopce_config();            /* -> grava g_dmg_config, desatualizado */
```

O arquivo ficava correto em formato e **velho em conteudo**: toda mudanca de Scale, Effect e
Screen voltava no valor anterior depois do reboot.

O `dmg_config_load()` mascara o resto, porque ele chama os `apply_*` e devolve `persistent_*`
corretos. Como o `g_dmg_config` estava certo para os campos que o proprio writer gravou, a
apariencia era "persistencia funciona as vezes".

Quem **nao** tinha o problema: **PicoWS** (`save_config()` sincroniza `cfg_*` antes de
gravar), **PicoGBC** (`save_emulator_config()` sincroniza `persistent_*`) e **PicoNES**
(`save_picones_config()` escreve a struct propria a partir de `persistent_*`).
Quebrados: **PicoSMS, PicoA26, PicoLINX, PicoNGP, PicoMD, PicoPCE**.

**Correcao:** cada emulador ganhou um par `*_save_config()` / `*_load_config()` de verdade, que
copia `persistent_*` -> `g_dmg_config` antes de gravar. E o `dmg_menu.c` deixou de chamar
`dmg_config_save()` direto, passando por `dmg_config_commit()`:

```c
void dmg_config_commit(const dmg_emulator_desc_t *desc) {
    if (desc->save_config != NULL) desc->save_config();
    else                          dmg_config_save(desc);
}
```

Isso tambem fecha um cruzamento que ainda existia: mudar Scale no submenu e depois Volume no
menu compartilhado **na mesma visita** gravava o Scale antigo por cima, porque a gravacao do
Volume vinha do `g_dmg_config` intacto.

### Bug 2 â€” O carregador lia lixo de pilha de arquivos curtos

`dmg_config_t loaded;` nao era inicializado, e `f_read()` nao preenche o que o arquivo nao
tem. Config gravado por build antigo e menor que a struct: **`I:\saves\ms\picosms.cfg` tem 44
bytes** contra 48 da `dmg_config_t`. A cauda ficava com o que estivesse na pilha.

Isto nao e teorico, esta nos cartoes agora:

| Arquivo | `extra_opt1` no disco | Interpretado |
| :--- | :--- | :--- |
| `pce/picopce.cfg` | `0x69 74 20 66` = `'it f'` | lixo |
| `md/picomd.cfg` | `0x61 64 65 72` = `'ader'` | lixo |
| `a26/picoa26.cfg` | `0x44 6F 6E 6B` = `'Donk'` | lixo |
| `lynx/picolynx.cfg` | `0x4E 83 29 08` | lixo |

O campo passou a carregar o overlay de FPS, e o teste e "diferente de zero" -- entao lixo ali
significa overlay **ligado sozinho**.

**Correcao:** `memset(&loaded, 0, sizeof(loaded))` antes do `f_read`, aviso no log quando o
arquivo e curto, e clamp em `turbo_mode`, `grid_effect`, `interleave` e `extra_opt1` -- que
recebiam qualquer valor sem limite e sao usados como booleano.

### Bug 3 â€” Magic incompativel, e a falha era silenciosa

`I:\saves\ms\picosms.cfg` e `I:\saves\sms\picosms.cfg` tem magic **0x534D5350**. O codigo do
PicoSMS espera **0x47475350**. Nao batem, entao o arquivo **nunca foi lido** -- nenhuma das
duas versoes. O resultado era sempre o caminho de fallback, que aplica os defaults de
compilacao sem tocar no arquivo.

Este e o caso mais enganoso dos tres, porque nao e "nao salvou": e "nao esta lendo". E
parecia exatamente o bug 1.

**Correcao:** o fallback agora **diz por que** no log, em vez de falhar calado:

```
[shared_menu] PicoSMS config magic 0x534D5350, expected 0x47475350 -- ignoring file, using defaults
```

### Onde grava e onde le

O caminho ja era o certo em todos (`0:/saves/<plataforma>/<nome>.cfg`), e por isso o bug nao
era de pasta:

| Emulador | Arquivo | `saves_dir` |
| :--- | :--- | :--- |
| PicoPCE | `0:/saves/pce/picopce.cfg` | `0:/saves/pce` |
| PicoSMS / PicoGG | `0:/saves/ms/picosms.cfg` / `0:/saves/gg/picogg.cfg` | por build |
| PicoMD | `0:/saves/md/picomd.cfg` | `0:/saves/md` |
| PicoNGP | `0:/saves/ngp/picongp.cfg` | `0:/saves/ngp` |
| PicoLINX | `0:/saves/lynx/picolynx.cfg` | `0:/saves/lynx` |
| PicoA26 | `0:/saves/a26/picoa26.cfg` | `0:/saves/a26` |
| PicoWS | `0:/saves/ws/picows.cfg` | `0:/saves/ws` |
| PicoGBC / PicoDMG | `0:/saves/gbc/picogbc.cfg` / `gb/picodmg.cfg` | por build |
| PicoNES | `0:/saves/nes/picones.cfg` (struct propria) | `0:/saves/nes` |

### Salvar ao abrir e ao fechar o menu, carregar ao entrar no jogo

O pedido e explicito: gravar ao acessar e ao sair do menu in-game, e usar o arquivo na entrada
do jogo.

- `dmg_ingame_menu()` agora chama `dmg_config_commit()` **na abertura** e em **todos os tres
  retornos** (Resume, Reset, Exit). Antes so gravava quando uma linha mudava, entao uma visita
  que nao mexe em nada deixava o arquivo para tras.
- Cada emulador chama `*_load_config()` **quando entra no jogo**, logo apos `load_cart_rom_file()`,
  e reaplica escala, efeito, interlace, brilho e volume no driver. Isso e seguro justamente
  porque a gravacao vem antes: o arquivo sempre reflete o que estava na tela quando o menu
  fechou.

Um `f_read` de 48 bytes por entrada de jogo e irrelevante para o tempo de carga (o `dmg_menu.c`
agora diz no log qual arquivo usou), e o arquivo vira a fonte da verdade em vez de um valor de
boot.

### Bugs adicionais corrigidos no caminho

- **`apply_interleave = NULL` no PicoPCE e no PicoA26.** Sem callback, `dmg_config_load()` nunca
  empurrava o valor guardado para a variavel do emulador, entao "Screen: Progressive |
  Interleave" voltava no default de compilacao em **todo** boot, independente de qualquer
  gravacao. Ambos ganharam callback.
- **O overlay de FPS nao persistia em lugar nenhum**, em nenhum dos 9: `dmg_config_t` nao tem
  campo de fps. Agora `extra_opt1` e o transporte, e `*_load_config()` o le e chama
  `apply_fps` na mao. PicoGBC e PicoWS ja usavam `extra_opt1` para paleta e nao foram
  tocados.

### Binarios

18 UF2 recompilados (9 x HW15 + HW16), todos limpos.

### Pendente de Validacao em Hardware

1. **Ciclo completo**: mudar Scale/Effect/Screen no submenu, sair do menu, sair do jogo, entrar em outro, e o valor ter de estar lÃ¡. E o caminho novo de `*_load_config()` na entrada.
2. **Cruzamento**: mudar Scale no submenu e Volume no menu na mesma visita, e conferir o arquivo pelo PC -- os dois tem de estar certos. Era o bug queotivou o `dmg_config_commit`.
3. **Le o log de boot**: os arquivos velhos do cartao (`ms/picosms.cfg` com 44 bytes, magic antigo) agora devem produzir a mensagem de diagnostico em vez de fallback mudo. O primeiro `save` depois disso reescreve um arquivo valido de 48 bytes.
4. **Overlay de FPS**: `extra_opt1` estava com lixo de pilha em 4 cartoes, entao o overlay pode ter ligado sozinho ate aqui. Confirmar que comecam desligados.
5. **Nao regrediu**: PicoWS, PicoGBC e PicoNES ja persistiam e nao devem ter mudado de comportamento.

---

## 28. PicoGBC/PicoDMG â€” Tela Preta ao Trocar Scale no Submenu (Aplica So ao Sair do Menu)

### Sintoma

Ao alternar `Scale` no submenu `Palette >`, a tela fica preta e aparece **apenas a linha onde
esta o cursor** (a propria opcao `Scale`). Movendo o cursor para as opcoes abaixo, cada linha
volta a aparecer uma a uma. Ao voltar ao menu in-game principal, tudo reaparece.

### Causa raiz

Uma unica linha, no fim de `mk_ili9225_set_scale_mode()` (`mk_st7789.c:1825`):

```c
    init_scale_x_map();
    init_scale_y_map();
    mk_ili9225_fill(0x0000);        /* <-- pinta o painel INTEIRO de preto */
```

Isso e correto quando o jogo vai comecar a se desenhar por cima, e errado enquanto ha um menu
em cima. O submenu chamava o setter **imediatamente** a cada mudanca e so redesenhava a linha
que acabara de mudar (`draw_gbc_image_row(0, true)`). Resultado: painel preto com uma linha
visivel -- exatamente o sintoma -- e cada movimento de cursor repintava mais uma linha.

Voltar ao menu in-game principal restaurava tudo porque esse caminho faz um redesenho completo
(`fill_screen_safe` + todas as linhas), nao porque fosse um conserto.

Confirmei que **nao** e o resto do submenu: `mk_ili9225_set_grid_enabled()` apenas escreve um
flag, e `apply_palette_mode()` so mexe no array de paleta. Nenhum dos dois toca na tela.

### Correcao

A escala passa a ser aplicada em `ingame_menu()`, depois que `dmg_ingame_menu()` devolve:

```c
static ingame_action_e ingame_menu(struct gb_s *gb) {
  s_active_gb = gb;
  ingame_action_e act = dmg_ingame_menu(&s_gb_desc, current_loaded_game_name);
  mk_ili9225_set_scale_mode(persistent_scale_mode);
  return act;
}
```

O submenu agora so altera o valor, grava a config e redesenha a linha.

Seguro nos tres retornos: `Resume` e `Reset` ja faziam `mk_ili9225_fill(0x0000)` e
des-estacionam o core do LCD logo em seguida, entao o jogo se desenha sobre um painel em branco
exatamente como antes. `Exit` vai para o seletor de ROMs, que redesenha a tela toda. E o setter
retorna cedo quando modo e janela GB nao mudaram, entao abrir o menu sem tocar em `Scale` custa
uma comparacao.

A config continua a ser gravada **dentro** do submenu, entao a escolha nao se perde se o console
for desligado enquanto o menu esta aberto.

### Nota: `gbc_apply_scale` continua aplicando na hora

O callback do descritor (`gbc_apply_scale`) foi mantido aplicando imediatamente, porque e o que
`dmg_config_load()` chama no boot e na entrada do jogo -- la o painel precisa da geometria nova
antes do primeiro quadro. So a interacao do submenu foi adiada.

### Binarios

| Alvo | HW15 | HW16 |
| :--- | :--- | :--- |
| **PicoGBC** | `PicoGBC.uf2` 428.032 B | `PicoGBC_hw16.uf2` 433.664 B |
| **PicoDMG** | `PicoDMG.uf2` 428.032 B | `PicoDMG_hw16.uf2` 433.664 B |

### Pendente de Validacao em Hardware

1. Alternar `Scale` varias vezes dentro do submenu: a tela do menu **nao pode piscar nem ficar preta**.
2. Trocar para as outras linhas do submenu e voltar: tudo deve permanecer visivel.
3. Sair com `B`: a nova escala tem de aparecer no jogo.
4. `Effect`, `Palette` e `Screen` continuam reagindo de imediato (nao foram alterados) e sem tocar na tela do menu.

---

## 29. Reset do Menu In-Game e Persistencia da Bateria â€” Levantamento nos 9 Emuladores

### Sintoma Original (PicoWS)

No menu in-game, `Reset` reiniciava o jogo **sem recarregar o `.sav`**. O mesmo
jogo, aberto pelo seletor de ROMs, carregava o save normalmente.

### O Que o Codigo Realmente Faz

A correcao inicial foi escrita com uma premissa que a leitura do core desmentiu:
`ws_memory_reset()` (`PicoWS/src/oswan/memory.c:151`) tem o `memset` de
`ws_staticRam` **comentado**:

```c
void ws_memory_reset(void) {
   memset(internalRam, 0, 0x10000);
   //	memset(ws_staticRam,0,0x10000);
}
```

Ou seja, `ws_reset()` **nao** apaga a SRAM do cartucho. O que ele apaga e
`internalRam` (64 KB), que o formato do `.sav` nao inclui.

Isso muda a leitura do bug: nao era "o reset destruiu o save", era "o reset nao
releu o save do disco". Ler o `.sav` de volta deixa o Reset identico a entrar pelo
seletor de ROMs, que e o que "reiniciar com meu progresso" tem de significar.

### Formato do `.sav` do PicoWS (nao mudar)

Os `.sav` do cartao tem **73728 B** = `ws_staticRam` (65536) + `externalEeprom`
(8192). `internalRam` **nao** esta no formato, e isso e o comportamento correto:
work RAM se perde no reset de um console real, so os saves apoiados no cartucho
sobrevivem. Decisao do usuario: **nao alterar o formato**.

### Levantamento nos 9 Emuladores

| Emulador | Reset preserva o save? | Mecanismo |
| :--- | :--- | :--- |
| **PicoWS** | **nao** (corrigido aqui) | so `ws_reset()`; faltava `ws_sram_load()` |
| **PicoSMS / PicoGG** | sim | `system_reset()` chama `system_load_sram()` internamente |
| **PicoMD** | sim | `reset_emulation()` nao toca em `gwsram` |
| **PicoNGP** | sim | `flash_commit()` antes de `reset()`; overlay e separado do `ram[]` |
| **PicoGBC / PicoDMG** | sim | nem `gb_init()` nem `gb_reset()` limpam `ram[]` (so VRAM) |
| **PicoA26 / PicoLINX** | n/a | nao existe save de bateria neste fluxo |
| **PicoPCE** | n/a | **HuCard nunca persiste SRAM** â€” ver lacuna abaixo |

### Lacuna Conhecida: PicoPCE nao persiste SRAM de HuCard

Nao e bug de reset, e um recurso que nunca existiu. `LoadCard()` so recarrega a
ROM. Nao ha leitura nem escrita de `.sav` para HuCard em nenhum lugar do
projeto â€” o unico `.sav` e `bram.sav`, do Arcade Card dos jogos de CD
(`main.cpp:2113`, `:2181`). Confirmado por `I:/saves/pce/`, que contem apenas
`picopce.cfg` e tres `.ss`, nenhum `.sav`.

Reset com HuCard perde o progresso da sessao porque `LoadCard()` reinicializa a
SRAM â€” mas como ela nunca foi gravada, nao ha o que perder no disco. **Decisao do
usuario: deixar como esta e documentar.**

## 30. Bug Latente no PicoWS: `io.c` usa `externalEeprom` sem incluir `memory.h`

### Sintoma

O build **HW16** (`build_core2350b.ps1`) falhava:

```
src/oswan/io.c:217:20: error: 'externalEeprom' undeclared; did you mean 'internalEeprom'?
src/oswan/io.c:363:13: error: 'externalEeprom' undeclared; did you mean 'internalEeprom'?
```

O build HW15 passava â€” e essa e a parte interessante.

### Causa Raiz

`io.c` usa `externalEeprom` em `cpu_readport()` e `cpu_writeport()`, mas **nao
inclui `memory.h`**, onde o array e declarado. A unica outra declaracao que
`io.c` alcanca e a mascara, declarada a mao na propria linha 22:

```c
#include "gpu.h"
#include "memory.h"          /* <-- faltava */
extern uint32 externalEepromAddressMask;
```

`memory.h` so e incluido por `ws.c` e `ws_audio.c`. O que mascara o erro no
HW15 e o fato de `ieeprom.h:12` fazer uma **definicao provisoria** de
`internalEeprom[]` num header â€” por isso o compilador sugere `internalEeprom` e
nao `externalEeprom`: e o unico dos dois que ele enxerga.

O HW15 so passava porque o `io.c.obj` estava **cacheado** no `build_bootloader/`.
Forcado o rebuild (`touch io.c`), o HW15 falha com o mesmo erro. O binario
gerado antes desta correcao estava, portanto, ligado contra um objeto velho.

### Correcao

`#include "memory.h"` em `io.c`. Compila limpo nas duas placas.

### Nota: codigo morto com o mesmo padrao

`ws.c` le e escreve **131072 bytes** em `externalEeprom`, que tem 8192
(`ws.c:418`, `:499`, `:647`, `:709`). Um overflow de 122880 bytes, da mesma classe
do bug de `write8psram` do PicoGBC (Â§24.1). Os quatro sitios estao dentro de
blocos `#if 0` herdados do Oswan de PC, portanto **nao e bug vivo** â€” mas e a
mesma armadilha para quem reativar esse codigo.

### Binarios

| Alvo | Arquivo | Tamanho |
| :--- | :--- | ---: |
| **RP2350 Plus** | `PicoWS.uf2` | 743.936 B |
| **Core2350B** | `PicoWS_hw16.uf2` | 756.736 B |

### Pendente de Validacao em Hardware

1. **PicoWS**: `Reset` no menu in-game tem de reiniciar o jogo **com o save
   carregado**, igual a entrar pelo seletor de ROMs.
2. `.sav` de 24576 B (ex: `Rockman_EXE_WS`) tem de continuar a carregar â€” o
   `ws_sram_load()` recebe o tamanho do arquivo e nao deve assumir 73728.
3. Jogo sem `.sav` nenhum: `Reset` tem de dar jogo novo, sem crash.
4. Confirmar que o `PicoWS.uf2` deste build (com `io.c` recompilado de verdade)
   nao introduz regressao â€” o binario anterior estava ligado com objeto stale.

---

## 31. Aviso de Sobrescrita de Savestate e Slots de Load State Vazios

### O que foi pedido

1. Ao salvar em um slot que **ja tem** savestate, mostrar `Savedata exists, overwrite?`
   com `(A) Yes | (B) Cancel`. `A` salva e volta ao jogo; `B` permanece no menu in-game.
2. Na linha `Load State`, os slots **sem** savestate devem aparecer em cinza opaco e
   **nao podem ser selecionados**: com save no 1 e no 3, o cursor pula do 1 direto
   para o 3; com save so no 1, ele nao sai do 1.
3. Ao descer para a linha `Load State`, o cursor deve cair no primeiro slot com save.
4. Ao carregar, mostrar `Load State?` com `(A) Yes | (B) Cancel`. `A` carrega e volta
   ao jogo; `B` permanece no menu in-game.

### A mascara de disponibilidade

Tudo depende de uma mascara de 4 bits, `s_state_avail_mask`, lida **uma vez** quando o
menu abre (`scan_state_slots()`). Nao ha rescan por repaint: salvar encerra a visita ao
menu, entao nada dentro de uma visita pode criar ou remover um state -- um probe por
repaint seriam 4 aberturas de arquivo a cada movimento de cursor para aprender a mesma
coisa.

Como a pergunta e "isto pode ser carregado", e nao "o arquivo existe":

```c
bool dmg_state_file_present(const char *path) {
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return false;
    const bool usable = (f_size(&fil) > 0);
    f_close(&fil);
    return usable;
}
```

**Zero bytes conta como ausente**, e ja existe um assim no cartao:
`I:\saves\md\Sonic_The_Hedgehog_(USA,_Europe)_2.ss` tem 0 bytes, deixado por um save
interrompido. Oferece-lo transformaria um slot visivel em um load que falha, ou pior, em
um restore parcial. `f_open` em vez de `f_stat` porque o tamanho exige abrir o arquivo de
qualquer forma, e nao depende de `FF_FS_STAT`, que nem todas as copias do FatFs do projeto
habilitam.

### Como cada emulador responde

`dmg_state_slot_exists()` tem dois caminhos:

- **Generico** (8 dos 9): probe em `<saves_dir>/<game>_<slot>.ss`, que e o que
  `dmg_build_state_path()` produz e portanto onde `save_state()` escreve. Confirmado
  contra `I:\saves`: `Super Mario Bros. 3 (USA) (Rev 1).nes` ->
  `Super_Mario_Bros._3_(USA)_(Rev_1)_1.ss`. Tenta tambem a forma sem prefixo de volume
  (`/saves/<sistema>/...`), porque e o fallback que todo emulador tem e que o
  `load_state()` realmente le -- sem isso a linha cinza apagaria um slot que carrega.
- **Callback `state_exists` no descritor** (2 casos, porque o nome do arquivo nao segue a
  convencao):
  - **PicoGBC / PicoDMG** (`gbc_state_exists`): o arquivo e nomeado pelo **header da ROM**,
    nao pelo arquivo da cartucho -- o mesmo jogo e `Tetris (USA).gb` na lista e `TETRIS` no
    state. Repete tambem o fallback legado sem numero do slot 1 (`<rom>.ss`), que existe de
    verdade no cartao: `0:/saves/gbc/DK_COUNTRY.ss`, `SHANTAE.ss`.
  - **PicoNES** (`nes_state_exists`): slot 1 tem o mesmo fallback legado
    (`0:/saves/nes/<game>.ss` e `/saves/<game>.ss`).

O criterio: se o probe for mais restritivo que o `load_state()`, a linha apaga um slot que
carrega; se for mais frouxo, oferece um que falha.

### A Linha Load State Desenhada Slot a Slot

A linha virou `draw_load_state_slots()`. Os quatro slots sao uma run fixa de caracteres
dentro da string que a linha ja era (`"Load State:   1 2 3 4"`), entao cada celula fica
numa coluna de glifo conhecida e carrega a propria cor. E o motivo de nao ser um
`snprintf`: **uma string tem uma cor**, e o ponto da linha e que os slots vazios parecam
vazios. Coluna `14 + 3 * (slot - 1)`, o mesmo espaÃ§amento de sempre.

O cinza e `dim_color()`, uma mistura do texto no fundo da linha a 45%. Nao um cinza novo
no `dmg_palette_t`, porque esse cinza teria de ser definido nas duas paletas (RGB565 e
RGB555) e poderia divergir entre elas; misturar as duas que ja existem nao pode. E usa o
fundo da linha, entao o cinza funciona tanto em branco (nao selecionada) quanto em menta
(selecionada).

### Navegacao

`step_load_slot()` anda para o proximo/anterior slot **que tem state**, dando a volta
dentro do conjunto dos que tem. Do 1 com save no 1 e no 3, direita vai para o 3 e depois
volta para o 1, sem nunca tocar no 2. Com um unico save, o cursor simplesmente fica.

`focus_load_slot()` roda ao entrar na linha e no inicio do menu, e so move o cursor se o
slot atual **nao** for valido. Deliberado: um slot que o jogador escolheu de proposito nao
deve ser movido a cada visita a linha. O que nunca pode acontecer e o cursor ficar parado
num slot vazio, que e desenhado cinza e onde nao se pode fazer nada.

### Dialogo de Confirmacao

`confirm_dialog()` desenha um cartao modal centralizado com titulo, linha de detalhe e as
duas respostas. **Espera a liberacao antes de ler**, nao depois de desenhar: o A que abriu o
cartao continua pressionado, e ler primeiro confirmaria o dialogo com o mesmo toque que o
convocou. B cancela e o menu e redesenhado por `redraw_ingame_screen()` (o mesmo helper que
o submenu customizado usa).

Load mostra `No Saved States` no status em vez do dialogo quando a mascara e zero: nao ha
nem um slot para oferecer.

### Binarios

18 UF2 recompilados (9 x HW15 + HW16), todos limpos.

| Alvo | HW15 | HW16 |
| :--- | :--- | :--- |
| **PicoA26** | `PicoA26.uf2` 489.472 B | `PicoA26_hw16.uf2` 500.224 B |
| **PicoGBC** | `PicoGBC.uf2` 428.032 B | `PicoGBC_hw16.uf2` 433.664 B |
| **PicoDMG** | `PicoDMG.uf2` 428.032 B | `PicoDMG_hw16.uf2` 433.664 B |
| **PicoLNX** | `PicoLNX.uf2` 427.008 B | `PicoLNX_hw16.uf2` 429.568 B |
| **PicoMD** | `PicoMD.uf2` 1.841.152 B | `PicoMD_hw16.uf2` 1.851.392 B |
| **PicoNES** | `PicoNES.uf2` 1.160.704 B | `PicoNES_hw16.uf2` 1.171.456 B |
| **PicoNGP** | `PicoNGP.uf2` 1.028.096 B | `PicoNGP_hw16.uf2` 1.038.848 B |
| **PicoPCE** | `PicoPCE.uf2` 657.408 B | `PicoPCE_hw16.uf2` 1.146.368 B |
| **PicoSMS** | `PicoSMS.uf2` 871.936 B | `PicoSMS_hw16.uf2` 883.712 B |
| **PicoGG** | `PicoGG.uf2` 870.912 B | `PicoGG_hw16.uf2` 882.688 B |
| **PicoWS** | `PicoWS.uf2` 743.424 B | `PicoWS_hw16.uf2` 755.712 B |

### Pendente de Validacao em Hardware

1. Salvar no 1, sair, voltar: a linha `Load State` mostra o 1 normal e os outros 3 cinza,
   e `A` no 1 abre o dialogo de sobrescrita.
2. `B` no dialogo de sobrescrita volta ao menu **com o menu inteiro desenhado** (nada de
   cartao residual) e sem ter salvo.
3. Salvar no 1 e no 3: entrar na linha cai num deles, e esquerda/direita alterna 1 <-> 3
   sem passar pelo 2 nem pelo 4.
4. Jogo sem nenhum state: `Load State` mostra a dica `No saved states` no rodape e `A`
   responde `No Saved States`.
5. **PicoGBC / PicoDMG**: `DK COUNTRY.gb`, que so tem o legado `DK_COUNTRY.ss`, tem de
   aparecer com o **slot 1 disponivel** -- e nao cinza, que seria o erro classico de um
   probe que esquece o fallback legado.
6. **PicoNES**: `Castlevania` tem so `CASTLEVANIA2_BEL_1.ss`; conferir o slot 1 como
   disponivel.
7. O slot 2 do Sonic no MD (0 bytes) tem de aparecer **cinza**, e nao falhar ao carregar.
8. Cancelar o load com `B` nao pode carregar o state nem fechar o menu.

---

## 32. Bug: Savestate gravado como `.s` â€” `char state_path[64]` era um byte curto

### Sintoma

No **PicoSMS**, salvar em um slot vazio funcionava, voltava ao jogo, e ao reabrir o menu
in-game a linha `Load State` continuava mostrando tudo indisponivel e o aviso de sobrescrita
nao aparecia. Visualmente correto, funcionalmente nada.

Nao era o codigo novo do Â§31. Era um **bug pre-existente** que o Â§31 expÃ´s.

### Causa raiz

O estado estava no cartao com o nome **errado**:

```
I:\saves\sms\Alex_Kidd_in_Miracle_World_(World)_(Sega_Ages)_1.s     <- 50 chars, ".s"
I:\saves\sms\Alex_Kidd_in_Miracle_World_(World)_(Sega_Ages)_1.ss    <- 51 chars, correto
```

`I:\saves` tem 48 arquivos `.ss` e esses dois `.s`. E o unico par nao-`.ss` do cartao
inteiro â€” nenhum outro sistema apresentava o problema.

A conta fecha exatamente:

```
"0:/saves/sms/"                            13
"Alex_Kidd_in_Miracle_World_(World)_(Sega_Ages)"  46   (base sanitizada)
"_1.ss"                                     5
                                            ---
                                             64 caracteres  ->  65 bytes com o NUL
```

E `sms_save_state()` usava:

```c
char state_path[64];
dmg_build_state_path(SAVES_DIR, game_name, slot, state_path, sizeof(state_path));
```

`snprintf` escreve 63 caracteres + NUL: **o ultimo `s` era descartado**. Faltava um byte.

### Por que save e load concordavam entre si, e o resto nao

Os dois usavam o mesmo buffer de 64, entao ambos truncavam do mesmo jeito: gravar e ler
funcionavam, e o round trip de save->load nunca deu erro. Foi o Â§31 que **introduziu uma
segunda computacao do mesmo nome** â€” o probe, com `char path[96]` â€” e ela pediu o nome
certo. Duas computacoes do mesmo caminho com criterios de truncamento diferentes e o
primeiro que "nao funciona" e o novo, que nao tem culpa nenhuma.

Este e o padrao que o Â§25 e o Â§26 ja denunciavam duas vezes (copias do mesmo codigo que
divergem em silencio). Aqui a divergencia nao era entre emuladores, era entre **duas
chamadas dentro do mesmo emulador**.

### Correcoes

1. **`DMG_STATE_PATH_MAX 96` em `dmg_menu.h`**, com a conta do pior caso no comentario:
   `"0:/saves/a26/"` (15) + nome sanitizado de 63 + `"_4.ss"` (5) + NUL = 84. O valor
   documentado e o minimo que qualquer chamador precisa.
2. **Buffers corrigidos** de `char state_path[64]` / `char legacy_path[64]` para
   `DMG_STATE_PATH_MAX` em **PicoSMS** (PicoSMS + PicoGG), **PicoNES** e **PicoPCE** â€” os
   tres unicos com 64. Os outros seis ja usavam `FF_MAX_LFN` (256) ou 256, e o PicoGBC
   tem 64 mas com nome de ROM de no maximo 15 chars (pior caso 34), entao nunca trunca.
3. **`dmg_build_state_path()` nao trunca mais em silencio.** Monta o caminho num buffer
   proprio de 192 bytes, mede o tamanho real e, se o buffer do chamador for pequeno,
   **imprime no log o nome pretendido** em vez de gravar um nome errado no cartao sem
   avisar.
4. **Probe = o que o `load_state()` realmente abre.** O caminho generico ficou com UM
   caminho so, `<saves_dir>/<game>_<slot>.ss`, que e exatamente o que sete dos nove
   `load_state()` abrem. Antes ele aceitava tambem a forma sem prefixo de volume
   (`/saves/<sistema>/...`), que e mais permissivo que o loader de sete deles â€” oferecer um
   slot que o load nao alcanca produz "Empty Load State" depois de o jogador confirmar.
   Quem tem fallback ganhou callback `state_exists`:
   - **PicoWS** â€” numerado em `0:/saves/ws` **e** em `/saves/ws` (`ws_state_exists`).
   - **PicoSMS/PicoGG** â€” numerado em `SAVES_DIR` + legado sem numero no slot 1 em
     `SAVES_DIR` e `SAVES_DIR_FALLBACK` (`/saves/ms`, `/saves/gg`) (`sms_state_exists`).
     Deliberadamente **nao** sonda o numerado em `SAVES_DIR_FALLBACK`: o loader nunca abre
     la, e oferecer seria exatamente o erro do outro lado.
   - **PicoPCE** â€” numerado + dois legados do slot 1 (`pce_state_exists`).
   - **PicoNES** e **PicoGBC** ja tinham o seu desde o Â§31.

### Recuperacao dos arquivos no cartao

Os dois arquivos orfaos foram **renomeados no proprio cartao** (`.s` -> `.ss`); o conteudo
do state e valido e so o nome estava errado. Nenhum state foi perdido.

### Binarios

8 UF2 recompilados (4 alvos x HW15 + HW16), todos limpos.

| Alvo | HW15 | HW16 |
| :--- | :--- | :--- |
| **PicoSMS** | `PicoSMS.uf2` 872.960 B | `PicoSMS_hw16.uf2` 884.736 B |
| **PicoGG** | `PicoGG.uf2` 871.936 B | `PicoGG_hw16.uf2` 883.712 B |
| **PicoPCE** | `PicoPCE.uf2` 658.432 B | `PicoPCE_hw16.uf2` 1.147.392 B |
| **PicoNES** | `PicoNES.uf2` 1.161.216 B | `PicoNES_hw16.uf2` 1.171.968 B |
| **PicoWS** | `PicoWS.uf2` 743.936 B | `PicoWS_hw16.uf2` 756.736 B |

### Os outros cinco: verificados e recompilados

A auditoria de todos os call sites de `dmg_build_state_path()` do projeto:

| Emulador | Buffer do chamador | Pior caso | Trunca? |
| :--- | :--- | ---: | :--- |
| **PicoSMS/GG, PicoNES, PicoPCE** | `state_path[64]` | 65 B | **sim** -> corrigidos acima |
| PicoMD, PicoA26, PicoNGP, PicoLINX | `path[FF_MAX_LFN]` = 255 | 84 B | nao |
| PicoWS | `path[256]` | 84 B | nao |
| PicoGBC / PicoDMG | `filename[64]`, mas nome do header tem no maximo 15 chars | 34 B | nao |

O pior caso global e `"0:/saves/a26/"` (15) + base sanitizada (63, teto do `char base[64]`)
+ `"_4.ss"` (5) + NUL = **84 bytes**. `FF_MAX_LFN` e **255 em todos** os `ffconf.h` do
projeto, entao os quatro que o usam tem folga de 171 bytes.

O truncamento, portanto, nao pode ocorrer neles. Eles foram recompilados mesmo assim porque
estavam uma revisao atras nas duas mudancas deste ciclo no `shared_menu` (o aviso de buffer
pequeno e o probe generico estreitado) — o probe antigo ainda aceitava a forma sem prefixo
de volume, que esses quatro `load_state()` nunca abrem.

### Teto de 63 chars no nome

`dmg_build_state_path()` limita a base a 63 chars (`char base[64]`), o que significa que uma
ROM com nome sanitizado maior tem o filename cortado. Isso e seguro **porque o corte esta em
um unico lugar**: save, load e probe passam pelo mesmo helper e produzem o mesmo nome. Nao ha
divergencia, apenas um nome mais curto. O pior caso real do cartao e o Alex Kidd, de 46.

### Binarios (todos os 18, duas revisoes)

| Alvo | HW15 | HW16 |
| :--- | :--- | :--- |
| **PicoA26** | `PicoA26.uf2` 489.984 B | `PicoA26_hw16.uf2` 500.736 B |
| **PicoGBC** | `PicoGBC.uf2` 428.032 B | `PicoGBC_hw16.uf2` 433.664 B |
| **PicoDMG** | `PicoDMG.uf2` 428.032 B | `PicoDMG_hw16.uf2` 433.664 B |
| **PicoLNX** | `PicoLNX.uf2` 427.520 B | `PicoLNX_hw16.uf2` 430.080 B |
| **PicoMD** | `PicoMD.uf2` 1.841.664 B | `PicoMD_hw16.uf2` 1.851.904 B |
| **PicoNES** | `PicoNES.uf2` 1.161.216 B | `PicoNES_hw16.uf2` 1.171.968 B |
| **PicoNGP** | `PicoNGP.uf2` 1.028.608 B | `PicoNGP_hw16.uf2` 1.039.360 B |
| **PicoPCE** | `PicoPCE.uf2` 658.432 B | `PicoPCE_hw16.uf2` 1.147.392 B |
| **PicoSMS** | `PicoSMS.uf2` 872.960 B | `PicoSMS_hw16.uf2` 884.736 B |
| **PicoGG** | `PicoGG.uf2` 871.936 B | `PicoGG_hw16.uf2` 883.712 B |
| **PicoWS** | `PicoWS.uf2` 743.936 B | `PicoWS_hw16.uf2` 756.736 B |

### Pendente de Validacao em Hardware

1. **PicoSMS, Alex Kidd**: salvar no slot 1, sair, voltar ao menu. O arquivo no cartao tem
   de se chamar `Alex_Kidd_in_Miracle_World_(World)_(Sega_Ages)_1.ss` â€” **`.ss`, com os dois
   `s`**. O slot 1 tem de aparecer disponivel e `A` no slot 1 tem de abrir o aviso de
   sobrescrita.
2. **O aviso de sobrescrita** no PicoSMS: slot 1 e 2 ja tem state (renomeados), slot 3 e 4
   estao vazios. Salvar no 3 tem de ir direto ao jogo; salvar no 1 tem de pedir confirmacao.
3. **Navegacao do Load State** com save no 1 e no 2: o cursor alterna 1 <-> 2 e nunca toca
   nos slots 3 e 4, que devem ficar cinza.
4. **Um jogo de nome longo em qualquer sistema** (o criterio e `len(pasta) + len(nome) +
   5 >= 64`): salvar, e conferir no PC que a extensao e `.ss`. Vale medir antes de
   escolher: o pior caso real do cartao e o Alex Kidd, de 64 chars.
5. Se aparecer a mensagem nova no log (`state path buffer too small`), o emulador tem algum
   outro lugar construindo caminho de state com buffer pequeno â€” a mensagem diz o nome
   pretendido e o tamanho necessario.

---

## 33. Cor da barra de selecao: verde menta -> vermelho #BF0000

### O pedido

A barra que marca a navegacao (a linha selecionada no seletor de ROMs e no menu in-game) era
verde menta. Passou a ser **#BF0000**.

### As duas paletas

O console nao tem uma cor so: o painel recebe RGB565 (16 bits) ou RGB555 (15 bits), conforme
`dmg_palette_t.is_rgb555`. As duas constantes foram convertidas do mesmo #BF0000:

| Formato | Conversao | Valor |
| :--- | :--- | ---: |
| RGB565 | `R = 0xBF >> 3 = 23 = 0x17` -> `0x17 << 11` | `0xB800` |
| RGB555 | `R = 23 = 0x17` -> `0x17 << 10` | `0x5C00` |

`DMG_RGB565_SEL_COLOR` era `0x3666` e `DMG_RGB555_SEL_COLOR` era `0x1B2C` — as duas eram
#33CC66. Uma unica mudanca nos dois lugares e todos os 9 emuladores mudam juntos, porque todos
derivam dessas macros: o seletor e o menu in-game vem do `shared_menu`, e os submenus
customizados (`Image >` / `Palette >`) tambem, direta ou por alias
(`ROM_UI_SEL_COLOR` no PicoNGP e no PicoLINX, `UI_SEL` no PicoWS). **Nenhum emulador tem
literal proprio** — o unico `0x3666u` hardcoded do projeto esta no `PicoSNES`.

### O texto sobre a barra tambem mudou

Este nao era pedido, e e a consequencia direta de ter obeyedido: **preto sobre #BF0000 e
ilegivel**.

| Texto sobre #BF0000 | Razao de contraste (WCAG) |
| :--- | ---: |
| preto (`pal.text`) | **3.2:1** — abaixo de 4.5:1 |
| branco | **6.5:1** |

A luminancia relativa sRGB de #BF0000 e 0.111; com preto a razao fica em 3.2:1 e com branco em
6.5:1. O texto preto vinha do verde menta, contra o qual ele era o par de maior contraste da
paleta. Trocar so a barra deixaria todas as linhas selecionadas em algo que da para ler
apenas inclinando o console.

Por isso `dmg_palette_t` ganhou `text_sel` (branco), com o mesmo cuidado que o `dim_color()`
dos slots vazios: um cinza novo teria de ser definido nas duas paletas e poderia divergir
entre elas; o branco ja existe nas duas.

Onde o texto passa a depender do fundo:

- `shared_menu/dmg_menu.c`: `draw_ingame_row()`, `draw_rom_item()` e `draw_load_state_slots()`.
- Os **9 submenus customizados**, um `*_text()` cada — foi necessario porque cada emulador
  desenha as proprias linhas por cima de `DMG_*_SEL_COLOR`.

A barra de progresso da tela de carregamento usa `pal.sel` como preenchimento e ficou
vermelha tambem. Nao ha texto sobre ela, entao nao foi mexido.

### O bootloader ficou de fora

`pico-bootLoader` nao linka `shared_menu` e tem paleta propria (`src/themes.cpp`,
`src/gui.cpp`). A barra dele continua verde, por decisao do usuario: e uma parte separada,
para mexer depois.

### Binarios

18 UF2 recompilados (9 x HW15 + HW16), todos limpos.

| Alvo | HW15 | HW16 |
| :--- | :--- | :--- |
| **PicoA26** | `PicoA26.uf2` 491.008 B | `PicoA26_hw16.uf2` 501.760 B |
| **PicoGBC** | `PicoGBC.uf2` 430.080 B | `PicoGBC_hw16.uf2` 435.712 B |
| **PicoDMG** | `PicoDMG.uf2` 430.080 B | `PicoDMG_hw16.uf2` 435.712 B |
| **PicoLNX** | `PicoLNX.uf2` 428.032 B | `PicoLNX_hw16.uf2` 430.592 B |
| **PicoMD** | `PicoMD.uf2` 1.841.664 B | `PicoMD_hw16.uf2` 1.852.416 B |
| **PicoNES** | `PicoNES.uf2` 1.163.776 B | `PicoNES_hw16.uf2` 1.174.528 B |
| **PicoNGP** | `PicoNGP.uf2` 1.029.120 B | `PicoNGP_hw16.uf2` 1.039.360 B |
| **PicoPCE** | `PicoPCE.uf2` 658.432 B | `PicoPCE_hw16.uf2` 1.147.392 B |
| **PicoSMS** | `PicoSMS.uf2` 875.520 B | `PicoSMS_hw16.uf2` 887.296 B |
| **PicoGG** | `PicoGG.uf2` 874.496 B | `PicoGG_hw16.uf2` 886.272 B |
| **PicoWS** | `PicoWS.uf2` 748.032 B | `PicoWS_hw16.uf2` 760.320 B |

### Pendente de Validacao em Hardware

1. A barra esta #BF0000 no seletor de ROMs e no menu in-game, nas duas placas (o ST7789 e o
   ILI9341 podem mostrar o vermelho com saturacao levemente diferente — e a primeira vez que
   uma cor saturada entra nesta UI).
2. O texto branco sobre a barra e legivel a distancia de uso normal.
3. Os slots vazios do `Load State` continuam legiveis sobre a barra vermelha: o `dim_color()`
   agora mistura branco no vermelho, o que da um rosa apagado em vez de um cinza.
4. Os submenus (`Image >` / `Palette >`) tambem ficaram com texto branco na linha vermelha.

---

## 34. Chave Liga/Desliga SPDT (Chave H) em Modo Ultra Baixo Consumo (Deep Sleep / Dormant)

### O Pedido
Implementar uma chave liga/desliga mecânica (chave H / SPDT) usando 2 dos 3 pinos disponíveis, sem cortar a linha de alimentação de energia (VBUS/VSYS), colocando o RP2350 e os periféricos em modo de baixíssimo consumo (DORMANT / Deep Sleep, consumo caindo de ~120mA para < 0.3mA), com retorno instantâneo (RESUME) ou reinício limpo (REBOOT).

### Ligação Física (Hardware Wiring)
Utiliza-se o pino **GP10** (pino físico 14 do barramento da Waveshare RP2350 Plus / Core2350B), que fica exatamente adjacente a um pino de **GND** (pino físico 13):
- **Terminal 1 da Chave**: Conectado ao pino físico 13 (**GND**).
- **Terminal 2 (Comum / Central da Chave)**: Conectado ao pino físico 14 (**GP10**).
- **Terminal 3 da Chave**: **Desconectado / Aberto** (NC).

**Comportamento elétrico com Pull-up interno ativado:**
- **Posição LIGADO (ON)**: Terminais 1 e 2 fechados -> GP10 conectado ao GND (Nível Lógico 0).
- **Posição DESLIGADO (OFF)**: Terminais 2 e 3 fechados (Terminal 1 aberto) -> GP10 em circuito aberto, puxado para 3.3V pelo pull-up interno (Nível Lógico 1).

### Configurações no `/boot.txt`
Compatível tanto com o bootloader (`pico-bootLoader`) quanto com todos os emuladores via `shared_menu`:

```ini
# Ativação da chave liga/desliga (0 = desativado por padrão, 1 = ativado)
POWER_SWITCH=1

# Pino GPIO utilizado (padrão: 10)
POWER_PIN=10

# Comportamento ao desligar: RESUME (suspende e retoma no ponto) ou REBOOT (reinicia para o bootloader ao ligar)
POWER_BEHAVIOR=RESUME

# Polaridade: 1 = GND é ON e Aberto é OFF (padrão); 0 = Aberto é ON e GND é OFF
POWER_ACTIVE_LOW=1
```

### Arquitetura de Software Implementada
1. **Módulo Compartilhado (`shared_menu/dmg_power.{h,c}`)**:
   - `dmg_power_init()`: Faz a leitura das configurações do `/boot.txt`, configura o GPIO com pull-up e estado inicial.
   - `dmg_power_poll(desc)`: Realiza polling com debounce de 30ms. Chamado a cada frame em jogo (`dmg_combo_frame`) e nos loops de menu (`dmg_read_ui_buttons`).
   - `dmg_power_enter_standby(desc)`:
     - Silencia áudio I2S e desliga backlight do display (`GP22 = 0`).
     - Envia comandos SPI `0x28` (DISPOFF) e `0x10` (SLPIN) para o display TFT (ILI9341 / ST7789).
     - Coloca o Core 1 em reset via `multicore_reset_core1()`.
     - Se `POWER_BEHAVIOR=REBOOT`: aguarda chave ir para ON e aciona `watchdog_reboot(0, 0, 0)`.
     - Se `POWER_BEHAVIOR=RESUME`: configura interrupção dormente (`gpio_set_dormant_irq_enabled`), suspende XOSC via `xosc_dormant()`.
     - Ao despertar: restaura clocks do sistema (`set_sys_clock_khz`), reativa o painel (`0x11` SLPOUT, `0x29` DISPON), reativa backlight (`GP22`) e reinicia blitter/Core 1 se necessário.
2. **Integração no Bootloader (`pico-bootLoader`)**:
   - Campos adicionados no parser de `sd_boot_ini.{h,c}` para não gerar erro de sintaxe.
   - Checagem da chave no loop principal do bootloader (`checkPowerSwitch`) antes de desenhar menus.
3. **CMakeLists.txt Unificado**:
   - Adicionada compilação de `dmg_power.c` e bibliotecas `hardware_xosc`, `hardware_rosc` e `pico_multicore` em todos os emuladores e no bootloader.

### Binários Compilados e Validados
- `pico-bootLoader.uf2`
- `PicoNES.uf2`
- `PicoSMS.uf2` e `PicoGG.uf2`
- `PicoMD.uf2`
- `PicoGBC.uf2` e `PicoDMG.uf2`
- `PicoA26.uf2`
- `PicoLNX.uf2`
- `PicoNGP.uf2`
- `PicoPCE.uf2`
- `PicoWS.uf2`

---

## 25. Splash Screen Personalizada no Bootloader (`pico-bootLoader`)

### Descrição e Contexto
Substituição da tela textual inicial (`"Scanning emulators..."` / `0:/emu` / carimbo de versão) por uma tela gráfica de apresentação:
1. **Fundo**: Branco puro (`#FFFFFF` / `0x0FFF` RGB444).
2. **Logotipo ArcadeFun (`Logo A.png`)**: Redimensionado com interpolação bicúbica para 97x88 pixels, com altura visual (88px) perfeitamente emparelhada com o QR Code.
3. **QR Code**: 33x33 módulos gerado com margem de 2 módulos e escala 3x3 por módulo (99x99 pixels, com matriz ativa de 87x87 pixels) apontando para `https://www.youtube.com/@arcadefunbr`.
4. **Linha de Texto**: `"Visit @arcadefunbr on Youtube"` renderizada na fonte de sistema 8x8 em cor carvão (`#222222`), perfeitamente centralizada abaixo do logo e QR Code.
5. **Duração**: Exibição inicial durante o scan de arquivos `.uf2`, com tempo mínimo garantido de **3 segundos**, pulável imediatamente ao apertar qualquer botão no gamepad.
6. **Compatibilidade com GB_MODE**:
   - `GB_MODE=0`: Tela cheia 320x240, centralizado no display.
   - `GB_MODE=1`: Viewport de 266x240 (x=45..310), com barras pretas laterais (0..44 e 311..319) mantidas pelo driver `ili9341`.

### Arquivos Criados e Modificados
- `pico-bootLoader/src/splash_assets.h` (novo): Arrays de bitmap RGB444 do logo (`splash_logo_arcadefun`, 97x88) e matriz booleana do QR (`splash_qr_arcadefun`, 33x33).
- `pico-bootLoader/main.cpp`: Implementação de `showSplashScreen()`, direcionamento explícito em `getActiveFramebuffer()` para `USE_ILI9341`, substituição de `showMessage("Scanning emulators...")` e loop de 3s pulável com qualquer botão. Inicialização com `memset` em `screenBuffer` para evitar renderização de lixo de memória RAM em `DrawScreen(-1)`.
- Binários atualizados: `pico-bootLoader/build_ili9341/pico-bootLoader.uf2`, `pico-bootLoader/build_hw16/pico-bootLoader.uf2` e `pico-bootLoader/pico-bootLoader.uf2`.

---

## 26. Customização da Tela de Flashing para Flash XIP no Bootloader (`pico-bootLoader`)

### Descrição e Contexto
Substituição da caixa textual clássica de gravação (`"Flashing"`, `<nome>`, `"Do not power off."`) por uma tela gráfica integrada de carregamento/gravação para a Flash XIP:
1. **Logo e QR Code**: Posicionados no topo da tela (`top_y = 16`). Logo ArcadeFun (97x88) à esquerda e QR Code (99x99) à direita.
2. **Chamada do Canal**: `"Scan QR to visit us on Youtube"` centralizada logo abaixo do Logo e QR (`y = 120`).
3. **Espaçamento de 2 Linhas**: Intervalo limpo de 24 pixels (`y = 128 .. 151`) separando o bloco de identidade visual do bloco de progresso.
4. **Nome da Plataforma/Emulador**: Exibido centralizado em `y = 152` (ex: `"PicoMD"`), identificando com clareza o sistema sendo gravado.
5. **Alerta de Segurança**: `"DO NOT POWER OFF"` centralizado em vermelho `#BF0000` (`y = 166`), exatamente acima da barra.
6. **Supressão de "Flashing"**: A palavra "Flashing" foi completamente removida.
7. **Barra de Progresso**: Redimensionada para `PB_Y = 180`, `PB_H = 14`, com preenchimento em vermelho `#BF0000` (`PB_COL_FILL = 0x0B00` em RGB444 / `0x5C00` em RGB555).
8. **Porcentagem (%)**: Centralizada logo abaixo da barra (`PB_LBL_Y = 198`), com respiro de 34 pixels na base da tela.
9. **Otimização de Barramento SPI durante Gravação**: `ili9341_flush_rows(179, 207)` atualiza via SPI DMA apenas as 28 scanlines da barra e da porcentagem durante o erase/program da Flash. O restante da tela permanece fixo e estático no framebuffer.

### Arquivos Modificados
- `pico-bootLoader/src/progress_bar.cpp`: Geometria atualizada para `PB_Y = 180`, `PB_H = 14`, `PB_LBL_Y = 198`.
- `pico-bootLoader/main.cpp`: Implementação de `drawFlashScreen(const char *emuName)`, definição de `PB_COL_FILL` em `#BF0000`, ativação no botão `A` e em `flashAndLaunch()`, e linhas de flush otimizadas `ili9341_flush_rows(179, 207)`.
- Binários atualizados: `pico-bootLoader/build_ili9341/pico-bootLoader.uf2`, `pico-bootLoader/build_hw16/pico-bootLoader.uf2` e `pico-bootLoader/pico-bootLoader.uf2`.

---

## 27. Filtro Anti-Aliasing (Sharp vs Shimmerless) Unificado, Correções de Submenu e Otimizações

### Contexto e Motivação
O filtro **Shimmerless** (Anti-Aliased Nearest Neighbor - AANN) introduzido no PicoGBC/PicoDMG interpola as bordas de pixels vizinhos com base na fração de subpixel quando a escala não é inteira, eliminando cintilações (shimmering) ao rolar a tela, sem causar o borrão excessivo de uma interpolação bilinear tradicional.

### Implementações Realizadas
1. **PicoNGP & PicoWS**:
   - Inclusão da opção `Filter: < Shimmerless >` / `< Sharp >` no submenu `Image` do menu in-game.
   - Implementação do algoritmo AANN no pipeline de renderização dos blitters (`ngp_render_slot_pixels` e `ws_band_push`/`ws_vband_push`).
   - Persistência em `extra_opt2` (`0u = Shimmerless` padrão, `1u = Sharp`).
2. **PicoNGP (Margens da Tela de Carregamento)**:
   - Substituída a rotina proprietária antiga (`draw_loading_screen`) por `dmg_draw_loading_screen()` e `dmg_update_loading_progress()` da biblioteca compartilhada `shared_menu`.
   - Quando `GB_MODE=1` no `boot.txt`, a barra e textos de progresso respeitam rigorosamente a janela visível de 266x240 ($x=45 \dots 310$) e preservam as bordas pretas laterais.
3. **PicoPCE (Eliminação de Queda de Frame)**:
   - Identificada sobrecarga causada por 3 divisões inteiras por pixel na interpolação.
   - Otimizado `shared_menu/dmg_scale.h` com blend paralelo de 32 bits livre de divisões com duas multiplicações:
     `(((c0 & 0xF81Fu) * inv_w5 + (c1 & 0xF81Fu) * w5) >> 5) & 0xF81Fu`.
   - Reestruturado `pce_band_push` no Core 1 com stores diretos de 16-bit (`__builtin_bswap16`) e caminhos dedicados para bypass de pixels não fracionados, restaurando 60 FPS estáveis.
4. **PicoMD (Estabilidade de Menu, Escala e Otimização 68k)**:
   - Corrigido travamento/encerramento do jogo ao alternar escala ou filtro: adiada a reconfiguração do blitter e a gravação no SD (`dmg_config_save`) para o botão `B: Apply & Back`.
   - Adicionada alimentação periódica de `watchdog_update()` durante a navegação do submenu.
   - Corrigidas opções de escala para o modo nativo 320x224 (ocultando Integer quando `GB_MODE=1`).
   - Módulos críticos do `gwenesis` promovidos para `-O3 -fno-strict-aliasing` no `CMakeLists.txt`.
5. **PicoLINX & PicoA26 (Conclusão do Rollout)**:
   - Ambos adaptados com suporte completo à seleção e persistência de `Filter: Sharp | Shimmerless` no submenu `Image`.
   - Adiada gravação de SD para o botão `B` com `watchdog_update()`.
   - Recompilados `PicoLNX_hw16.uf2` e `PicoA26_hw16.uf2`.

---

## 42. Indicador de Bateria (Divisor Resistivo + ADC) no `shared_menu`

### Ligacao (Core2350B, TP4056 com protecao)
```
B+ (= OUT+) --- R1 100k 1% ---+--- GP40 (ADC0)
                              |       |
                          R2 100k   C 100 nF
                              |       |
OUT- (GND da placa) ----------+-------+
B-  : so a celula. Nada do console liga aqui (o MOSFET de protecao fica entre B- e OUT-).
```
- RP2350B: ADC so em GP40..GP46 (GP47 = PSRAM CS). GP26..29 NAO sao ADC nesta variante.
- Razao 1/2: 4,2 V -> 2,1 V no pino. Consumo permanente ~21 uA.
- O corte do DW01 (~2,4 V) e baixo demais para a celula; o firmware avisa em vermelho <= 20%.

### Chave `BATT_PIN=` no `/boot.txt`
- `BATT_PIN=40` liga a leitura; ausente/0 = desligado (padrao, HW15 inalterado).
- `shared_menu/dmg_panel.c`: parse + `dmg_panel_get_batt_pin()`.
- `pico-bootLoader/src/sd_boot_ini.{h,c}`: chave registrada na whitelist (sem isso o arquivo
  inteiro seria rejeitado). Corrigido junto: `rumble_pin` nao tinha default no parser.

### Modulo `shared_menu/dmg_battery.{h,c}`
- `dmg_battery_init_pin(pin)`: aceita so `ADC_BASE_PIN .. ADC_BASE_PIN + NUM_ADC_CHANNELS - 2`
  e recusa PSRAM CS e pinos I2S do build; `adc_gpio_init()` (buffer digital off, erratum E9).
- `dmg_battery_read_mv()`: descarta 1 conversao apos o mux, media de 16, `raw * 3300 * 2 / 4095`.
- < 2500 mV = sem celula (so USB) -> indicador escondido.
- `dmg_battery_percent()`: curva de tensao **em repouso**, interpolacao linear:
  3300=0, 3450=5, 3550=10, 3650=20, 3720=30, 3770=40, 3810=50, 3860=60, 3910=70, 3960=80,
  4010=90, 4060=100. A tabela antiga punha 100% em 4,20 V (tensao de carga do TP4056); uma
  celula carregada a noite toda le ~4,08 V pelo divisor depois de desligar o carregador, e o
  marcador parava em 88% (validado em hardware).
- Nao depende de `dmg_panel`, para o bootloader poder compilar o mesmo arquivo na etapa 2.

### Marcador na barra superior (canto direito)
- So no seletor de jogos (`Page N`) e no menu de pausa (Start+B, `Pause Menu`).
  `dmg_draw_top_bar()` publico continua sem bateria (tela de carregamento e submenus intactos).
- `"87%"` + icone 18x10 com terminal 2x4; preenchimento e texto em `pal.sel` (#BF0000) <= 20%.
- Reserva de 56 px a direita; o subtitulo e alinhado antes dela. Respeita GB_MODE (x=45..310).
- Reamostra a cada 2 s nos loops dos dois menus; redesenha a barra so quando o % muda.
  O ADC nunca e lido durante o gameplay. Log: `[battery] <mV> mV -> <pct>%`.

### Etapa 2 (pendente)
Barras superior/inferior no modo lista do `pico-bootLoader`, no estilo do seletor do
`shared_menu`, reaproveitando `dmg_battery.c`.

### Pendente de Validacao em Hardware
1. `BATT_PIN=40` no `/boot.txt`: o bootloader aceita o arquivo (sem `BOOT.TXT INVALID`).
2. Seletor e menu de pausa mostram o icone; o mV no log UART bate com o multimetro no B+.
3. GB_MODE=1: indicador dentro da janela, sem sobrepor `Page N` / `Pause Menu`.
4. So USB, sem celula: indicador some.


## 43. Modo BOOTSEL so no `pico-bootLoader` (SELECT + START + UP na partida)

- Segurar **SELECT + START + UP** enquanto liga o console reinicia o RP2350 em USB BOOTSEL
  (`reset_usb_boot(0, 0)`), para gravar UF2 pelo drive `RPI-RP2` sem abrir a carcaca.
- `pico-bootLoader/main.cpp`: `checkBootselComboAtPowerOn()`, chamada logo apos
  `setClocksAndStartStdio()` e **antes** do salto de resume, do display, do SD e do USB. Uma
  imagem ruim mais adiante nao bloqueia a recuperacao. Le direto GP2 (UP), GP8 (SELECT) e
  GP9 (START), com pull-up e 6 amostras em ~30 ms contra bounce. So nos builds `USE_ILI9341`
  (HW15 e HW16).
- PicoNES: removido o acorde in-game SELECT+START+UP+A -> BOOTSEL (`main.cpp`). O item 5 da
  validacao do §25 deixa de valer. Nenhum outro emulador tinha atalho de BOOTSEL ativo.
- O menu antigo `pico_shared/menu.cpp` ("Enter BOOTSEL Mode") nao e compilado pelo bootloader.

### Pendente de Validacao em Hardware
1. Segurar SELECT+START+UP e ligar: o PC mostra o drive `RPI-RP2`.
2. Ligar sem o combo: boot normal, inclusive o resume para o ultimo emulador.
3. SELECT+START+UP durante um jogo no PicoNES nao faz mais nada alem dos combos normais.

### Bug de build corrigido: `build_ili9341.ps1` gerava o bootloader DVI
O script so rodava `ninja` em `build_ili9341/`, cujo cache tinha virado `HW_CONFIG=2` /
`USE_ILI9341=0`. O UF2 "ILI9341" era na verdade o loader DVI, sem nada do console (nem
`BATT_PIN`, nem o combo de BOOTSEL). Agora o script configura explicitamente
`HW_CONFIG=15`, `USE_ILI9341=1`, `PICO_BOARD=waveshare_rp2350_plus_16mb` numa pasta limpa,
`build_hw15/`.

| Alvo | Arquivo | Tamanho |
| :--- | :--- | ---: |
| **HW15** (RP2350 Plus / ILI9341) | `pico-bootLoader/build_hw15/pico-bootLoader.uf2` | 603.136 B |
| **HW16** (Core2350B / ST7789) | `pico-bootLoader/build_hw16/pico-bootLoader.uf2` | 609.280 B |


## 44. Bateria: corte em 3,3 V, overlay no jogo e indicador de carga

### Deteccao do carregador (`CHARGE_PIN=41`)
```
IN+ do modulo TP4056 (5 V do USB = pino 4, VCC, do CI)
 |
 R3 100k (ou 10k)
 |
 +-------- GP41
 |
 R4 100k (ou 10k) -- igual ao R3
 |
OUT- (GND da placa)
```
- USB ligado: 2,5 V no GP41 (nivel alto); solto: 0 V. Entrada digital sem pull.
- Nao usa o pino CHRG: com o USB solto o TP4056 fica sem alimentacao e o CHRG pode ser puxado
  para ~0 V pelas protecoes internas, o que daria falso "carregando".
- Carregando = USB e < 4150 mV (raio pulsando); USB e >= 4150 mV = completa (raio fixo).
  Na fase CV do TP4056 a celula ja marca ~4,2 V, entao "completa" pode aparecer um pouco antes do fim.
- Chave registrada em `dmg_panel.c` (`dmg_panel_get_charge_pin()`) e na whitelist do bootloader.

### Corte em 3,3 V
- `DMG_BATTERY_CUTOFF_MV` em 3 amostras seguidas (~6 s) e sem carregador.
- **No jogo:** `dmg_combo_frame()` chama `dmg_battery_game_tick()`; no corte ele levanta
  `request_exit_game` (o emulador sai pelo caminho normal e grava o save) e o menu onde ele cai
  mostra o aviso e entra no standby. Assim a tela e redesenhada direito ao acordar.
- **Nos menus:** `battery_tick()` mostra o cartao "Battery low / Charge the battery" por ~2 s e chama
  `dmg_power_enter_battery_standby()` (mesma sequencia do standby da chave, refatorada em
  `standby_sleep()` / `standby_wake()`).
- Acorda com o carregador conectado ou ciclando a chave OFF -> ON. `POWER_BEHAVIOR=REBOOT` reinicia.
- Ao acordar sem carregador, so libera acima de `DMG_BATTERY_RELEASE_MV` (3,4 V).

### Marcador nos menus
- Vermelho (`pal.sel`) <= 20%; verde #1E8C3A com raio 6x8 quando no carregador (o verde puro do
  `status_ok` nao se le na barra clara). Raio pulsa a cada 500 ms redesenhando so o marcador.

### Overlay no jogo (<= 20%, sem carregador)
- API propria (`dmg_batt_overlay_visible/get_row/generation/y0/h`), separada do HUD de volume:
  varios drivers usam a borda liga/desliga do HUD de volume para limpeza e forcam reenvio de 180..199.
- Area 64x14 em y=3..16, alinhada a borda direita do marcador dos menus. **Sem cartao/moldura**: so
  `"NN%"` e o icone em vermelho, com contorno preto de 1 px colado nos glifos (legivel sobre cenas
  vermelhas/claras) e o interior do icone escuro; o resto e transparente (0x0001).
  Renderizado no core 0 em buffer duplo, publicado com `__dmb()`.
- Helpers para os drivers (`dmg_menu.h`):
  - `dmg_batt_overlay_stamp_row(y, buf, buf_x0, buf_w, swap)`: carimba a linha ja renderizada.
  - `dmg_batt_overlay_row_salt(y)`: misturar no hash da linha, para reenviar ao aparecer/mudar/sumir.
  - `dmg_batt_overlay_push_borders(gx0, gx1, gy0, gy1, push)`: drivers de quadro inteiro (core 1);
    envia a parte fora da imagem sobre preto a cada quadro, e uma passada preta ao esconder.
  - `dmg_batt_overlay_mark_row()` / `dmg_batt_overlay_frame_rows()`: drivers de fila com hash;
    o jogo marca as linhas que emite e no fim do quadro as linhas do overlay fora da imagem sao
    enfileiradas como linhas pretas com o overlay (cobertura de 2 quadros por causa do interleave,
    reenvio a cada 30 quadros para sobreviver a limpeza de tela dos menus).
- Por driver:

| Emulador | Como |
| :--- | :--- |
| PicoNES | piloto: carimbo + `nes_push_batt_overlay_borders()` proprio |
| PicoPCE | carimbo + `push_borders` (envia as 240 linhas, so as colunas fora da janela) |
| PicoA26 | carimbo + `push_borders` (Fit/Integer comecam em y=24) |
| PicoGBC/DMG, Pico8 | carimbo nos 1/3 caminhos de `mk_st7789.c` + `push_borders` apos o DMA |
| PicoNGP | carimbo + salt no hash do core 1; `push_borders` no `NGP_TAG_FRAME_END` (Integer 44..195, largura `s_active_x0`) |
| PicoSNES, PicoMD, PicoLINX | salt + linhas do overlay em largura total + linhas sinteticas no fim do quadro |
| PicoSMS/GG | carimbo antes do hash + linhas sinteticas via `sms_band_push(NULL..)` |
| PicoWS | salt + largura total nas duas orientacoes; linhas FPS do fim do quadro tambem carimbadas |

- **Bug corrigido junto (PicoSMS/GG):** em Integer, o fim do quadro enviava linhas pretas 180..199
  por cima da imagem enquanto o HUD de volume estava ativo (SMS cobre 24..215). Agora so as linhas
  fora da imagem (GG: 192..199).

### Pendente de Validacao em Hardware
1. GP41: ~2,5 V com USB, 0 V sem. Menus: verde com raio pulsando ao carregar.
2. Fonte de bancada: 3,6 V -> marcador vermelho e overlay no PicoNES; 3,25 V por > 6 s -> sai do jogo,
   aviso, standby; carregador ou ciclo da chave -> acorda.
3. PicoNES em Stretch, Fit e Integer (GB_MODE 0 e 1): overlay inteiro, sem lixo ao sumir. [OK]
4. Marcador chega a 100% depois de carga completa e desconectar (curva nova).
5. Overlay sem moldura nos 11 emuladores, em Stretch/Fit/Integer, GB_MODE 0 e 1, e WS em Tate:
   aparece inteiro, some sem deixar resto nas bordas, sem queda de FPS.


## 45. Standby por Savestate (chave OFF = salvar + DORMANT, ON = abertura + load state)

Camada comum no `shared_menu`. O que e especifico de cada emulador (driver, audio,
onde o `_R` e carregado, correcoes dos testes) esta na secao
"Standby por Savestate e Resume" do `agents.md` de cada um. A parte do bootloader
esta no §11 do `pico-bootLoader/AGENTS.md`.

### Por que
O §34 dizia "DORMANT, < 0,3 mA", mas desde o §7 o standby dos emuladores era um laco
WFI acordado: tela apagada, Core 1 girando a 360 MHz no blitter, I2S rodando, PLLs e
vreg 1,30 V ligados. O DORMANT de verdade tinha sido abandonado porque restaurar PLL,
QMI e clk_peri em cada emulador travava a CPU. Como agora a volta passa por um reboot,
nada precisa sobreviver ao DORMANT alem do `scratch[5]` do watchdog, e o problema de
restaurar clocks deixa de existir.

Medido pelo usuario (multimetro, Core2350B):

| | Jogando | Standby antigo | Standby novo |
| :--- | ---: | ---: | ---: |
| corrente | ~170 mA | ~75 mA | **0 a 2 mA** |

### Fluxo
**OFF** (emulador que chamou `dmg_power_enable_resume()`):
1. No jogo, `dmg_combo_frame()` levanta `ev.request_power_off` (+ `ev.power_reason`);
   o emulador age no limite de quadro: para o blitter e chama
   `dmg_power_off_with_resume(desc, reason)`. No menu de pausa e no seletor,
   `dmg_power_poll()` faz o mesmo.
2. `standby_sleep()`: backlight apagado, painel pintado de preto
   (`display.fill_screen(0)`; a GRAM sobrevive ao SLPIN, ao DORMANT e ao reset),
   DISPOFF/SLPIN.
3. No jogo: `save_state(game, DMG_RESUME_SLOT)` -> `<saves_dir>/<jogo>_R.ss` (o Load
   State 1..4 nunca mostra), `flush_saves()` (ou `exit_game()` se nulo) e
   `0:/saves/resume.bin` (app, pasta, ROM, nome do state).
4. `scratch[5] = 0x5E5A6Axx` (byte baixo: tipo GAME/MENU | motivo << 4).
5. `dormant_until()`: reset do Core 1, `watchdog_disable()`, clk_ref/clk_sys no XOSC,
   ADC/USB/HSTX parados, PLLs e ROSC desligados, vreg no padrao, `xosc_dormant()`.
   Acorda por nivel no GPIO da chave (ou no `CHARGE_PIN`, no corte de bateria).
6. ON: debounce (um bounce para OFF dorme de novo) e `watchdog_reboot`.
- No seletor: mesmo caminho, sem savestate (tipo MENU), volta ao seletor.
- Bateria (§44): o corte em 3,3 V tambem salva e dorme. Se acordar sem carregador e
  abaixo de 3,4 V, `dmg_resume_check()` volta a dormir sem tocar no jogo.

**ON**:
1. `pico-bootLoader`: com a marca no `scratch[5]`, pula direto para o app, sem
   abertura e sem init do display. Em qualquer outro boot zera o `scratch[5]`.
2. Emulador: `dmg_resume_check()` le a marca e o `resume.bin` (o `app` tem de bater
   com o `app_title`) e restaura a pasta de ROMs. GAME: `dmg_boot_intro_start()` e
   carga da ROM; MENU: `dmg_resume_consume()`, abertura e a lista.
3. `dmg_boot_intro_finish()` antes do primeiro acesso do jogo ao painel e ao I2S.
4. O `_R` e carregado **dentro do loop de quadros, depois do 1o quadro, pela mesma
   sequencia do Load State do menu de pausa** do emulador (`s_resume_pending`;
   modelo: `InfoNES_LoadFrame()` do PicoNES). Carregar antes do loop, fora desse
   caminho, deixou PicoMD mudo e PicoSMS/GG e PicoWS lentos.
5. Falha (ROM sumiu, state ausente): jogo novo ou seletor, sem travar.

### Modulos novos
- **`dmg_power.{h,c}`** (refeito): `dmg_power_enable_resume`, `dmg_power_resume_enabled`,
  `dmg_power_set_game(rom_file, game_name)` (guarda ponteiros, nao copias),
  `dmg_power_note_pause_menu`, `dmg_power_switch_off_confirmed`,
  `dmg_power_off_with_resume`, `dmg_power_sleep_and_reboot`,
  `dmg_power_enter_battery_standby`. Sem `enable_resume`, o comportamento e o antigo.
- **`dmg_resume.{h,c}`**: `dmg_resume_write_record`, `dmg_resume_set_mark`,
  `dmg_resume_check(desc, &rec)` -> NONE / MENU / GAME, `dmg_resume_consume`.
- **`dmg_boot_intro.{h,c}`**: a abertura do bootloader (mesmo asset
  `../pico-bootLoader/src/boot_anim_assets.h`, mesmas temporizacoes e mesmo som) no
  **Core 1**, enquanto o Core 0 carrega. `dmg_boot_intro_start/active/finish`,
  `dmg_boot_intro_flash_begin/end`.
  - comeca pintando 320x240 de preto (uma coluna por vez, buffer na pilha do Core 1);
  - som de 22,05 kHz levado a 44,1 kHz, ganho pelo `VOLUME=` global;
  - `finish()` espera a abertura, reseta o Core 1, espera o anel de audio esvaziar
    (le `audio_free()` ate estabilizar, no maximo 200 ms) e chama `audio_restore()`;
  - `flash_begin()` termina a abertura e libera o Core 1 antes de gravar flash
    (`flash_end()` e no-op). Sem lockout multicore: gravar flash num resume e raro
    (todos comparam a ROM antes) e o lockout custava RAM que o PicoWS nao tem.

### Contrato com o emulador
`dmg_display_t`:
- `write_rect_565(x, y, w, h, px)`: escrita direta no painel, RGB565 host, SPI
  bloqueante. Sem ele a abertura nao roda.

`dmg_emulator_desc_t` (campos novos, no fim):
- `audio_free()` / `audio_push(l, r)`: espaco livre **real** e envio de uma amostra
  a 44,1 kHz. Responder um valor fixo num driver que descarta o excesso faz o som da
  abertura sair com tom errado (foi o caso do PicoGBC e do Pico8).
- `flush_saves()`: grava os saves de bateria antes de dormir (nulo -> `exit_game()`).
- `audio_restore()`: chamado no fim da abertura; deixa o audio como o emulador espera
  (taxa, anel e DMA). Nos drivers de anel EXT e `audio_i2s_flush()` (abort + limpa a
  IRQ pendente + zera indices) + `audio_i2s_update_pio_frequency(<taxa do jogo>)`.

`dmg_combo_events_t`: `request_power_off`, `power_reason`.

Regras para o emulador enquanto a abertura roda:
- o Core 0 nao toca no painel: `dmg_draw_loading_screen()` e
  `dmg_update_loading_progress()` retornam cedo, e desenho proprio no load checa
  `dmg_boot_intro_active()`;
- gravacao de flash entre `dmg_boot_intro_flash_begin()` e `_end()`;
- no RP2350, `dma_channel_abort()` de um DMA ativo ainda pode disparar a IRQ de fim:
  limpar `ints0/ints1` depois do abort, senao o handler avanca o indice do anel logo
  apos o reset.

Para migrar um emulador: `write_rect_565` no driver, `audio_free/push/restore`,
`flush_saves`, `dmg_power_enable_resume()` + `dmg_resume_check()` no boot,
`request_power_off` no limite de quadro, load do `_R` no loop pelo caminho do menu de
pausa, `dmg_power_set_game()` em volta de cada jogo.

### Outras mudancas no `shared_menu`
- `dmg_menu.c`: `DMG_RESUME_SLOT` (0) monta o nome `_R.ss`; `dmg_ingame_menu()`
  envolve o menu com `dmg_power_note_pause_menu`; o corte de bateria usa
  `off_with_resume`; o `VOLUME=` do `boot.txt` e regravado linha a linha, sem o 2o
  buffer de 1 KB (RAM do PicoWS HW15).
- `dmg_combo.c`: o corte de bateria no jogo levanta `request_power_off` quando o
  resume esta ligado.
- `CMakeLists.txt` dos emuladores: `dmg_resume.c` e `dmg_boot_intro.c` adicionados
  (o `dmg_menu.c` passou a depender deles).

### Emuladores migrados
Todos: PicoNES (piloto), PicoSMS/GG, PicoA26, PicoLINX, PicoNGP, PicoMD, PicoPCE
(HuCard e CD/CHD), PicoWS, PicoGBC/DMG, PicoSNES, Pico8. Detalhes e correcoes de
cada um no respectivo `agents.md`.

| Emulador | Taxa do jogo | `audio_restore` |
| :--- | ---: | :--- |
| NES, SMS/GG, A26, NGP, MD, PCE, WS | 44,1 kHz | `audio_i2s_flush()` + 44,1 kHz |
| PicoSNES | 32 kHz | `audio_i2s_flush()` + `SNES_AUDIO_RATE` |
| PicoLINX | 22,05 kHz (abertura decimada) | espera o ultimo `i2s_dma_write` |
| PicoGBC/DMG, Pico8 | 44,1 kHz | `i2s_audio_start()` |

### Binarios
22 UF2 (11 alvos x HW15 + HW16), todos limpos. PicoWS HW15 em 99,96% da RAM.

| Alvo | HW15 | HW16 |
| :--- | ---: | ---: |
| `PicoNES/PicoNES.uf2` | 1287168 | 1297920 |
| `PicoSMS/PicoSMS.uf2` | 999424 | 1011712 |
| `PicoSMS/PicoGG.uf2` | 999424 | 1011200 |
| `PicoA26/PicoA26.uf2` | 605184 | 616448 |
| `PicoLINX/PicoLNX.uf2` | 560128 | 563200 |
| `PicoNGP/PicoNGP.uf2` | 1154560 | 1165824 |
| `PicoMD/PicoMD.uf2` | 1971712 | 1982976 |
| `PicoPCE/PicoPCE.uf2` | 786944 | 1276416 |
| `PicoWS/PicoWS.uf2` | 895488 | 954368 |
| `PicoGBC/PicoGBC.uf2` | 551936 | 558080 |
| `PicoGBC/PicoDMG.uf2` | 551936 | 558080 |
| `PicoSNES/PicoSNES.uf2` | 1282560 | 1287168 |
| `Pico8/Pico8_hw15.uf2` | 762368 | 762880 |

### Status
Validado em hardware (Core2350B) pelo usuario em 2026-11-05, em todos os emuladores:
OFF no jogo, no menu de pausa e no seletor; volta com tela preta, abertura com o som
certo e o jogo no mesmo ponto, com audio e FPS normais.
