# Plano de Implantação — Placa 16: Waveshare Core RP2350B + PSRAM 8 MB + ST7789

**Data:** 29/09/2026
**Escopo:** migração do console PicoDMG para a placa **Waveshare Core 2350B** (RP2350B, 16 MB flash, 8 MB PSRAM), com **carregamento de ROMs do microSD direto para a PSRAM** e **suporte a display ST7789 320x240** selecionado por `boot.txt`.

**Restrição permanente do projeto:** console portátil, **controles exclusivamente por GPIO (GP2..GP9)**, ativar em 0. Nenhuma referência a controles USB/externos deve ser introduzida ou mantida no caminho da Placa 16.

---

## Status — Fase 1 implementada em 29/09/2026

### Decisões confirmadas pelo usuário

| Item | Decisão |
|:--|:--|
| `INVERT=1` | Aplica `CMD_INVON`; `INVERT=0` aplica `CMD_INVOFF` |
| `ST7789_XOFF` / `ST7789_YOFF` | **Mantidos** como diagnóstico opcional, padrão `0/0` |
| `MADCTL=0xNN` | **Adicionada** após o 1º teste em hardware; ajuste cru e opcional do registro `0x36`, tem precedência sobre `ROTATE` (ver §2.5) |
| Botões | `GP2..GP9` (idem `HW_CONFIG 15`), lidos direto por GPIO |
| LED de status | `LED_BUILTIN` do Core2350B = **GPIO 39** (não 25) |

### O que foi implementado

| Arquivo | Mudança |
|:--|:--|
| `pico_shared/BoardConfigs.cmake` | Bloco `HW_CONFIG 16`: `PICO_BOARD=waveshare_core2350b` (`FORCE`), `PSRAM_CS_PIN=47`, pinos SPI do display e do SD |
| `CMakeLists.txt` | `PICO_BOARD` só é forçado quando `HW_CONFIG != 16`; novo `-DDEFAULT_PANEL_ST7789` (padrão `0`, preserva os builds antigos) |
| `src/sd_boot_ini.h` / `.c` | Chaves `ST7789`, `ROTATE`, `MADCTL`, `INVERT`, `ST7789_XOFF`, `ST7789_YOFF` com defaults vindos de `DEFAULT_PANEL_ST7789`, flags `seen_*`, rejeição de duplicata/valor inválido, e gravação no `sd_boot_ini_save()` |
| `src/drivers/ili9341/ili9341.c` / `.h` | `dmg_panel_t` + `ili9341_set/get_panel()`, `ili9341_set/get_invert()`, `ili9341_set/get_gram_offset()`, `ili9341_apply_default_rotation()`. Sequências separadas `ili9341_init_sequence()` / `st7789_init_sequence()` |
| `pico_shared/FrensHelpers.h` / `.cpp` | `Frens::setPreDisplayHook()`, chamado por `initAll()` entre `initSDCard()` e `initDVandAudio()`. `initDVandAudio()` passou a usar `ili9341_apply_default_rotation(1)`, que **não sobrescreve** uma rotação escolhida antes do power-on |
| `main.cpp` | `applyBootPanelConfig()` registrado como hook; faz o parse de `/boot.txt` **antes** do power-on e **reaproveita** o resultado depois de `initAll()` (sem releitura do cartão) |
| `build_core2350b.ps1` | `-DUSE_ILI9341=1 -DHW_CONFIG=16 -DDEFAULT_PANEL_ST7789=1` → `build_hw16\pico-bootLoader.uf2` |

Sequência ST7789 = a mesma de `PicoGBC\src\firmware\mk_st7789.c:592-617` (mesmos pinos CS/DC/RST/BL), não uma variante inventada.

### Matriz de compilação

| Build | Comando | Resultado |
|:--|:--|:--|
| `HW_CONFIG 15` (regressão ILI9341) | `-DUSE_ILI9341=1 -DHW_CONFIG=15` | **OK** — `pico-bootLoader.uf2`, 542 KB |
| `HW_CONFIG 16` (Core2350B/ST7789) | `build_core2350b.ps1` | **OK** — `pico-bootLoader.uf2`, 548 KB; FLASH 280504 B / 16 MB, RAM 192144 B / 512 KB (36,65%) |
| DVI | `-DUSE_ILI9341=0` | **OK** — 555 KB |

Zero warnings novos nos arquivos alterados.

### Desvios e pendências

- **USB/XInput mantido na Fase 1 — decisão explícita do usuário (29/09/2026).** A remoção
  prevista na seção 1.2 do plano não é possível sem refatoração: `pico_shared/hid_app.cpp`
  referencia `tinyusb_host` e `xinput_host`, e `tusb_xinput/xinput_host.c` é compilado.
  As 4 chamadas a `tuh_task()` em `main.cpp` seguem ativas. A restrição "sem controles
  externos no caminho da Placa 16" continua valendo — apenas os botões GP2..GP9 estão
  **ligados**; o host USB segue apenas como código herdado inerte. A eliminação real fica
  para uma fase própria, com conditional compilation.
- **`GB_MODE` já existia** no driver (`ili9341_set_gb_mode()`), aplicado em
  `main.cpp` **depois** de `initAll()`. Funciona porque MADCTL e a janela podem ser
  reprogramadas com o painel aceso, então não foi necessário mover nada.
- **PSRAM ainda não é usada pelo bootloader** (região `PSRAM: 0 B` no mapa de
  link). A inicialização da PSRAM própria em `0x11000000` é escopo das Fases 2/3.
- **Premissas de hardware a validar fisicamente:** fiação SD (SPI1 12–15) e TFT
  (SPI0 17–22) assumidas idênticas ao harness da `HW_CONFIG 15`; GPIO 39 para o
  LED; o header `waveshare_core2350b.h` **não** define `PICO_SMPS_MODE_PIN`, e
  `setClocksAndStartStdio(252000, 1.30V)` é sensível a isso.
- **Perguntas ainda abertas (não bloqueiam a Fase 1):** teto de ROM 4 vs 7 MB,
  clock do SD 20 vs 32 MHz, escopo do PicoWS.

---

## 0. Sumário Executivo

| # | Frente | Escopo | Risco |
|:--|:--|:--|:--|
| **F1** | `pico-bootLoader` | Placa 16, PSRAM 8 MB, `boot.txt` com `ST7789`/`ROTATE`/`INVERT` | Médio |
| **F2** | `shared_menu` | API de painel (`dmg_is_st7789_mode`, etc.) + API de ROM em PSRAM | Baixo |
| **F3** | 9 emuladores | ROM microSD → PSRAM (eliminar gravação da ROM na flash) | **Alto** |
| **F4** | 9 emuladores | Driver de display com ramo ST7789 | Médio |
| **F5** | Validação | Matriz de regressão por emulador | — |

**Decisões estruturais (resumidas):**
1. Criar `HW_CONFIG = 16` (Placa 16) em `BoardConfigs.cmake` — não alterar a `HW_CONFIG 15`, que continua servindo a placa antiga.
2. Trocar `PICO_BOARD` para `waveshare_core2350b` (o header **já existe** no SDK 2.3.1) — obrigatório, pois a `HW_CONFIG 15` é **RP2350A** (GPIO 47 não é bonded) e sem isso a PSRAM é fisicamente inacessível.
3. Painel **selecionado em runtime** por `boot.txt`, com painel padrão por compilação como fallback.
4. **Caminho de flash preservado como fallback automático** se o JEDEC da PSRAM falhar — o console nunca deixa de funcionar.
5. Headers de cache de ROM (PicoMD/NGP/WS/LYNX/GBC) são **descartados no caminho PSRAM**: sem cache, sem header, sem verificação por amostra.

---

## 1. Diagnóstico Consolidado

### 1.1 Placa

| Propriedade | `waveshare_rp2350_plus_16mb` (atual, `HW_CONFIG 15`) | `waveshare_core2350b` (novo) |
|:--|:--|:--|
| Variante | `PICO_RP2350A 1` — **RP2350A** | `PICO_RP2350A 0` — **RP2350B** |
| GPIO disponíveis | 0..29 | **0..47** |
| Flash | 16 MB `W25Q080` | 16 MB `W25Q080` (idêntico) |
| PSRAM CS | inexistente | **GPIO 47** (`PICO_PSRAM_CS_PIN 47`) |
| Detecção auto de PSRAM | — | `PICO_AUTO_DETECT_PSRAM_SIZE 1` |
| LED | — | GPIO 39 |
| `PICO_SMPS_MODE_PIN` | 23 | não definido |
| SPI0 padrão | SCK18/TX19/RX16/CS17 | **idêntico** |
| UART0 | TX0/RX1 | **idêntico** |

Header verificado em `C:\Users\Michael\.pico-sdk\sdk\2.3.1\src\boards\include\boards\waveshare_core2350b.h`.

> **Consequência dura:** GPIO 26–29 e 39+ **não existem** no RP2350A. A PSRAM só é alcançável trocando o `PICO_BOARD`. Não há caminho alternativo.

### 1.2 Infraestrutura de PSRAM — **já existe e está apenas desligada**

O projeto **não** usa o `pico_psram` do SDK. Usa uma implementação própria:

| Arquivo | Papel |
|:--|:--|
| `pico_shared/drivers/pico_psram/SetupPsram.c` | Bring-up QMI direto, probe JEDEC `0x9F`, exige `kgd == 0x5D` (família APS6404) |
| `pico_shared/drivers/pico_psram/PicoPlusPsram.cpp` | Singleton; `#define PSRAM_LOCATION 0x11000000`; entrega a região ao **lwmem** |
| `pico_shared/drivers/lwmem/` | Alocador sobre a região |

Fatos verificados:

- `SetupPsram.c:19` — `gpio_set_function(psramCS, GPIO_FUNC_XIP_CS1)` reivindica o CS1.
- `SetupPsram.c:188-201` — tamanho por JEDEC `eid`: base 1 MiB; `eid==0x26 || size_id==2` → **8 MiB** (detectado automaticamente).
- `SetupPsram.c:140-163` — teto de `max_psram_freq = 133 MHz`; força divisor maior conforme `clk_sys`.
- `SetupPsram.c:204` — `xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS`.
- `SetupPsram.c:207` — teste de escrita no **alias não-cacheado `0x15000000`**.
- `PicoPlusPsram.cpp:23-27` — se o CS configurado falhar, **tenta GPIO 47 automaticamente**.

**Por que a escrita funciona:** no RP2350, `XIP_CTRL.WRITABLE_M0=0 / WRITABLE_M1=1` torna **toda a janela SSI `0x10000000..0x1FFFFFFF` gravável**, incluindo `0x11000000` onde a PSRAM mora. O próprio `pico_psram` do SDK faz exatamente o mesmo (`psram.c:284`). Por isso `Frens::f_malloc` já pode devolver ponteiros em PSRAM graváveis — o bootloader já usa isso para themes/screensaver (`main.cpp:1703-1706`).

**Estado atual (por que está desligado):**

- `pico_shared/BoardConfigs.cmake:625` → `set(PSRAM_CS_PIN 0 CACHE STRING ...)` dentro de `HW_CONFIG 15`. Valor `0` ⇒ todos os blocos `#if PICO_RP2350 && PSRAM_CS_PIN` compilam para o **stub**.
- Nos emuladores: `PSRAM_CS_PIN:STRING=0` em todos os `build_bootloader/CMakeCache.txt` (PicoA26, PicoMD, PicoNES, PicoNGP, PicoPCE, PicoSMS, PicoWS). PicoGBC e PicoLINX **não têm** a variável.
- O bootloader **já chama** `Frens::initPsram()` em `FrensHelpers.cpp:1788` dentro de `initAll()`. Basta o CS correto para acionar.
- `Frens::flashromtoPsram()` **já está implementada e completa** em `pico_shared/FrensHelpers.cpp:1061-1225` (SD → PSRAM, com CRC32, byteswap, guarda para imagens grandes, buffer de rascunho de 4 KB). Só está atrás do `#if` morto.

### 1.3 Layout de flash — **inalterado, já correto**

`pico_shared/BootPartition.cmake` (fonte única de verdade, incluído em `CMakeLists.txt:98`):

```
0x10000000  +----------------------+  XIP base
            |  pico-bootLoader     |  512 KB
0x10080000  +----------------------+  FRENS_APP_BASE
            |  Partição de apps    |  15.5 MB  <-- offset dos emuladores
0x11000000  +----------------------+
```

`BoardConfigs.cmake` não é o dono do mapa; `BootPartition.cmake:103-104` força `PICO_FLASH_SIZE_BYTES=0x1000000` (=16 MB), **sobrescrevendo** o header da placa. 16 MB da Placa 16 == 16 MB da placa atual ⇒ **nenhuma mudança necessária** no mapa de flash, e o offset `0x10080000` dos emuladores continua válido.

### 1.4 Display

| Item | Bootloader | Observação |
|:--|:--|:--|
| Driver | `pico-bootLoader/src/drivers/ili9341/ili9341.c` (461 linhas) | Alvo `ili9341` em `src/drivers/CMakeLists.txt:6` |
| Framebuffer | `s_fb[320*240]` `uint16_t` RGB**444** `0x0RGB` (`:22`) | ~150 KB SRAM |
| Conversão | `rgb444_to_rgb565()` (`:257-266`) | replicação de bits |
| SPI | `spi_init(30 MHz)` → `spi_set_baudrate(62 MHz)` (`:136`, `:222`) | |
| DMA | 1 canal, `DMA_SIZE_16`, DREQ do SPI (`:268-279`) | |
| Comandos | `SWRESET`, `PWCTR1..5`, `VMCTR1`, `MADCTL=0x28`, `COLMOD=0x55`, `FRMCTR1`, `DFUNCTR`, `GAMSET`, `INVCTR`, `GMCTRP1/N1`, `SLPOUT`, `DISPON` (`:116-229`) | **100% ILI9341** |
| Rotação | `ili9341_set_rotation()` — **tabela por painel**, ver 2.5a/2.5b/2.5c. ILI9341: `0→0x08`, `1→0x28`, `2→0xC8`, `3→0xE8` (inalterado). ST7789: `0→0x40`, `1→0x60`, `2→0x80`, `3→0xA0` | `initDVandAudio` chama `rotation(1)` |
| GB_MODE | Barras pretas por scanline em `flush()` (`:313-332`) e `flush_rows()` (`:369-388`) | 0..44 e 311..319 |

**Reutilizável sem alteração no ST7789:** `MADCTL`, `COLMOD 0x55`, `CASET/PASET/RAMWR`, framing SPI 16-bit, DMA, e **todo** o caminho de GB_MODE (é conceito de framebuffer, não de painel).

**A substituir para ST7789:** `0xB1/0xB2/0xB3` (FRMCTR), `0xB4` (INVCTR), `0xB6` (DFUNCTR), `0x26` (GAMSET) — não existem. `0xC0/0xC1/0xC3/0xC4/0xC5` têm **semântica diferente** no ST7789 (`VCOM/VDV`, `VRHS/VDVS`, `VCO`). `0xE0/0xE1` têm layout diferente.

**Referência ST7789 já validada em hardware neste projeto:** `PicoGBC/src/firmware/mk_st7789.c` tem uma implementação ST7789 completa (2.107 linhas) com `ST7789_X_OFFSET`/`ST7789_Y_OFFSET` = 0, `ST7789_MADCTL_VALUE` = 0x28, `ST7789_COLMOD_VALUE` = 0x55, `ST7789_INIT_INVERT`, `ST7789_RGB_ORDER`, `ST7789_SWAP_RGB565_BYTES`, `st7789_remap_rgb565()` e blitter DMA por bandas. Auditoria de bordo (`PicoGBC/AGENTS.md` §12, §24) confirma que **MADCTL 0x28 em paisagem com offset 0/0** é o valor correto para um ST7789 320x240 deste tipo de módulo. Isso é a evidência de que **X/Y offset = 0 é o padrão correto**.

### 1.5 `boot.txt` — parser atual

`pico-bootLoader/src/sd_boot_ini.c` / `.h`. `sd_boot_ini_load()` (`.c:35`) faz defaults, adota `<path>.tmp` órfão (`.c:60-69`), valida duplicidade e emite diagnóstico de linha única.

| Chave | Padrão | Referência |
|:--|:--|:--|
| `BASEDIR` | `/emu` | `.c:43` |
| `INDEX` | `emulators.txt` | `.c:44` |
| `SCREENSAVER` | `STARFIELD` se ausente; `BLOCKS` se presente sem a chave | `.c:45`, `.c:83` |
| `GUI` | `1` | `.c:46` |
| `THEME` | `0` | `.c:47` |
| `GB_MODE` | `0` | `.c:48` |
| `VOLUME` | `3` | `.c:49` |

`sd_boot_ini_save()` reescreve **somente** `GUI=`/`THEME=` (e `GB_MODE`), copiando todo o resto byte a byte — bom: as chaves novas passam intactas. **Mas** quando precisa criar o arquivo do zero, escreve o valor efetivo de *todas* as chaves ⇒ precisa conhecer as novas.

### 1.6 Ordem de boot atual (bloqueante para a seleção de painel em runtime)

`pico-bootLoader/main.cpp:1473` → `main()`:

| # | Ação | Linha |
|:--|:--|:--|
| 1 | `setClocksAndStartStdio(252000, 1.30V)` + cache da capacidade JEDEC da flash | `1475` → `FrensHelpers.cpp:2091-2112` |
| 2 | banner / causa do boot | `1477-1488` |
| 3 | salto de retomada | `1495-1509` |
| 4 | `FrensSettings::initSettings(MULTI)` | `1513` |
| 5 | `Frens::initAll(...)` | `1535` |
| 5a | `initLed()` | `FrensHelpers.cpp:1780` |
| 5b | **`initPsram()`** | `FrensHelpers.cpp:1788` |
| 5c | `initSDCard()` + `f_mount(&fs, "", 1)` | `FrensHelpers.cpp:1822`, `:714` |
| 5d | `framebuffer = ili9341_get_framebuffer()` | `:1845-1846` |
| 5e | `initDVandAudio()` → **`ili9341_init()` + `ili9341_set_rotation(1)`** | `:1857` → `:1712-1713` |
| 6 | clamp do fim da partição de apps (JEDEC) | `1554-1559` |
| 7 | **`sd_boot_ini_load("/boot.txt")`** | `1576-1581` |
| 8 | `ili9341_set_gb_mode(ini.gb_mode)` | `1587-1590` |

> **Problema:** o painel é decidido em `5e`, mas `boot.txt` só é lido em `7`. Escolher o painel em runtime **exige** ler o `boot.txt` entre `5c` e `5e`.
> **Restrição da 1a:** `storage_get_flash_capacity()` precisa rodar **antes** de `initPsram()` e **antes** do overclock — já é o caso (passo 1a).

### 1.7 Controles

- Bootloader: `main.cpp:523-544` — GP2..GP9, active-low, pull-up interno, dentro de `#if USE_ILI9341`. **Fixo, sem parametrização.** GP2..GP9 existem no RP2350B ⇒ nenhuma alteração de código, apenas verificação de fiação.
- `shared_menu`: `dmg_init_ui_buttons()` / `dmg_read_ui_buttons()` em `dmg_menu.c:50-73`, GP2..GP9. Idem.
- USB/externo a remover do caminho da Placa 16: `tinyusb_host`, `tinyusb_board`, `xinput_host`, `tusb_xinput` (linkados em `CMakeLists.txt:282-295`, `add_subdirectory(tusb_xinput xinput_host)` em `:304`) e 4 chamadas a `tuh_task()` em `main.cpp:474`, `:511`, `:937`.

### 1.8 Carregamento de ROM por emulador (a frente mais delicada)

| Emulador | Loader | Destino hoje | Padrão | Já tem ramo PSRAM? |
|:--|:--|:--|:--|:--|
| **PicoA26** | `load_cart_rom_file` `main.cpp:379` | flash (local) | **B** | ❌ |
| **PicoGBC/DMG** | `main.c:1906-1913` | `FLASH_TARGET_ABS` (const `uint8_t *`, zero-copy) | **C** | ❌ |
| **PicoLINX** | `load_cart_rom_file` `src/main.cpp:453` | `XIP_BASE + FLASH_TARGET_OFFSET` (`uintptr_t`, alocado pós-`__flash_binary_end`) | **C** | ❌ |
| **PicoMD** | `load_cart_rom_file` `main.cpp:566` | `MD_ROM_CACHE_ABS` = `0x10080000 + 1 MB` | **B** | ❌ |
| **PicoNES** | `Frens::flashrom` `FrensHelpers.cpp:1275` | `ROM_FILE_ADDR` | **A** | ✅ `flashromtoPsram()` `:1061` |
| **PicoNGP** | `load_cart_rom_file` `src/main.cpp:590` | `NGP_ROM_CACHE_ABS`; `rom.data` é `uint8_t*` **gravável** | **B** | ❌ |
| **PicoPCE** | `load_cart_rom_file` `main.cpp:1034` | `PCE_ROM_CACHE_ABS` | **A/B** | ✅ parcial (`main.cpp:1400,1768,1790,1846`) |
| **PicoSMS/GG** | `flashrom` local `main.cpp:579` | `ROM_FILE_ADDR` | **A/B** | ❌ |
| **PicoWS** | `flash_load_rom` `src/main.cpp:380` | `WS_ROM_CACHE_ABS`; `ws_init((uint8_t*)...)` | **B** | ❌ |

- **Padrão A (5 projetos com `pico_shared` idêntico):** `PicoA26`, `PicoMD`, `PicoNES`, `PicoPCE`, `PicoSMS` — `FrensHelpers.cpp` com numeração de linha **byte-idêntica** entre as cópias.
- **Padrão B:** carregadores próprios em `main.cpp` que gravam direto na flash e **nunca chamam** `Frens::flashromtoPsram()`.
- **Padrão C:** `const`/endereço cru no XIP, zero-copy (`PicoGBC`, `PicoLINX`).

**Só 2 de 9 já têm ramo PSRAM.** Habilitar `PSRAM_CS_PIN` sozinho não muda nada nos outros 7.

**PicoGBC e PicoLINX não possuem `pico_shared/`** — precisam do driver de PSRAM + lwmem portados para dentro deles.

**Pressão de SRAM (restritiva, define o ganho do PSRAM):**

| Emulador | SRAM livre | Benefício de mover a ROM |
|:--|--:|:--|
| PicoWS | **~30 KB (92,09% usado)** | **Alto** — libera o cache de ROM; deve reduzir a fragmentação |
| PicoNGP | ~22,8 KB | **Alto** — idem |
| PicoPCE | 124 KB (75,64%) | Médio |
| demais | folgado | Baixo (o ganho é de **tempo**, não de SRAM) |

**Histórico relevante:** `PicoWS/AGENTS.md` documenta uma **remoção anterior de PSRAM** no PicoWS, feita por pressão de SRAM do core NEC V30 (`ws_staticRam[0x10000]` + `externalEeprom[0x2000]` foram movidos para SRAM interna). Com 8 MB de PSRAM, essa decisão fica **reversível** e o alvo correto passa a ser devolver esses buffers à PSRAM também (F3.9).

**Tamanhos máximos de ROM hoje (`*_ROM_CACHE_MAX`):** MD 4 MB, NGP 4 MB, WS 4 MB, LYNX 4 MB. Com 8 MB de PSRAM o teto sobe para ~7 MB (reservando margem para o lwmem e saves).

### 1.9 Ganho de velocidade esperado (a motivação do usuário)

Caminho atual, ROM de 4 MB:
1. Ler 4 MB do SD a ~20 MHz (SPI1) — **~1,7 s**
2. Apagar setores de 64 KB na flash — **~0,8 s**
3. Programar páginas de 256 B, 16 KB por rajada, com `memcmp` de verificação — **~1,5 s**
4. Recalcular divisor QMI e `watchdog_update()` por bloco

Caminho novo: apenas o passo 1, com escrita direta num buffer de 4 MB na PSRAM a ~126 MHz. **Elimina ~2,3 s por troca de jogo** e elimina o header de cache, as 32 amostras de verificação e a gravação de metadados.

---

## 2. Arquitetura Decidida

### 2.1 `HW_CONFIG = 16` — "Waveshare Core RP2350B (PicoDMG)"

Novo bloco em `pico_shared/BoardConfigs.cmake`, **imediatamente após** o bloco `HW_CONFIG 15` (termina em `:626`), mantendo a `HW_CONFIG 15` intacta.

Divergências em relação à 15, apenas:

| Variável | HW 15 | **HW 16** | Motivo |
|:--|:--|:--|:--|
| `PSRAM_CS_PIN` | `0` | **`47`** | CS da PSRAM do Core 2350B |
| `ILI9341_PIN_*`, `SD_*` | — | **inalterados** | Mesma fiação física do console |
| `NES_*`, `WII_*` | `-1` | `-1` | Sem controles externos (reforça a instrução do projeto) |
| `LED_GPIO_PIN` | `25` | `25` | Mantido; ver 4.1 |
| `USE_ILI9341` | `1` | `1` | Renomeado conceitualmente para "SPI TFT"; ver 2.2 |
| `UART_ENABLED` | `1` | `1` | Mantido (stdio de debug por UART0) |

Adicionalmente, `PICO_BOARD` deve passar a `waveshare_core2350b` com `FORCE` (`pico-bootLoader/CMakeLists.txt:61`).

`HW_CONFIG` é numérico e aparece na resolução de diretório (`main.cpp:1595`: `snprintf(hwDir, ..., "%s/%d", ini.base_dir, HW_CONFIG)`) ⇒ os emuladores da Placa 16 passam a viver em **`0:/emu/16/`**, exatamente como pedido. Fallback automático para `0:/emu/` se `16/` não existir (já implementado, `main.cpp:1597-1601`).

### 2.2 Seleção de painel em runtime

`USE_ILI9341` deixa de significar "use ILI9341" e passa a significar **"use TFT via SPI0"** (renomear a variável para `USE_SPI_TFT` com alias retrocompatível). O controlador concreto vem de `boot.txt`.

**Fator de inicialização (1ª chamada) — idêntico para os dois painéis:**
`gpio_init` (CS/DC/RST/BL) → `spi_init(30 MHz)` → `GPIO_FUNC_SPI` (MOSI, SCK) → reset de hardware (10 ms / 120 ms).

**Fator de 2 — sequência de power-on:**
```
ILI9341:  SWRESET(150ms) → PWCTR1/2 → VMCTR1 → MADCTL → COLMOD
          → FRMCTR1 → DFUNCTR → GAMSET → INVCTR → PWCTR3/4/5
          → VMCTR1 → GMCTRP1 → GMCTRN1 → SLPOUT(120ms) → DISPON(120ms)

ST7789:   SWRESET(150ms) → SLPOUT(120ms) → COLMOD 0x55 → MADCTL
          → {INVON|INVOFF} → set_addr_window(0,0,319,239)
          → NORON(10ms) → DISPON(120ms)
```
Essa é exatamente a sequência de `PicoGBC/src/firmware/mk_st7789.c:592-617`, já validada.

**Fator de 3 — `spi_set_baudrate(62 MHz)`** (mantém o valor; reavaliar contra o datasheet do painel; ST7789 tipicamente tolera até 62,5 MHz).

**API nova do driver** (`pico-bootLoader/src/drivers/ili9341/ili9341.h`):

```c
typedef enum { DMG_PANEL_ILI9341 = 0, DMG_PANEL_ST7789 = 1 } dmg_panel_t;

void      ili9341_set_panel(dmg_panel_t panel);   /* antes de ili9341_init() */
dmg_panel_t ili9341_get_panel(void);
void      ili9341_set_invert(bool on);            /* 0x21 INVON / 0x20 INVOFF */
bool      ili9341_get_invert(void);
```

`ili9341_set_rotation()` mantém a tabela existente e passa a ser aplicada sobre o painel selecionado.

**Mapeamento `ROTATE`:**

| `ROTATE` | Orientação | `MADCTL` | Nota |
|:--|:--|:--|:--|
| `0` | paisagem 0° | `0x28` | `MV|BGR` — **idêntico ao comportamento atual** |
| `1` | paisagem 180° | `0xE8` | `MX|MV|BGR` — reusando `case 3` da tabela existente |

`0x28 | 0x80 | 0x40 = 0xE8` ⇒ `ROTATE=1` é literalmente a rotação de 180° de `ROTATE=0`. Nenhuma tabela nova.

**Mapeamento `INVERT`:**

| `INVERT` | Comando | Significado |
|:--|:--|:--|
| `0` | `0x20 INVOFF` | estado normal (padrão) |
| `1` | `0x21 INVON` | **corrige** telas IPS com cores negativas |

> ⚠️ **Ponto a confirmar com o usuário:** a especificação diz "caso as cores estejam invertidas / `INVERT=0`". Adotoi `INVERT=1` = *aplicar a correção de inversão* (semântica padrão de todos os demos Waveshare/Pico SDK). Se a intenção for o inverso, é inverter uma constante.
>
> O driver ILI9341 atual **não emite** `0x20/0x21` na init (usa `0xB4 INVCTR=0x00`). Será necessário **adicionar** o par `INVON/INVOFF` explícito para que o knob funcione também no ILI9341.

**Fallback sem SD:** se o `boot.txt` não puder ser lido (cartão ausente), o painel assume o padrão de compilação `DEFAULT_PANEL` (`-DDEFAULT_PANEL_ST7789=0|1`; padrão `0` = ILI9341, preservando a placa antiga). Sem isso, um cartão removido deixaria o bootloader com o painel errado e a tela de erro fatal ilegível.

**Cache de XIP:** após a inicialização, emitir `xip_cache_clean_all()` / `xip_cache_inv_all()` para invalidar qualquer linha em cache da região do controlador.

### 2.3 Ordem de boot corrigida

Novo **hook** em `pico_shared/FrensHelpers.cpp`, inserido em `initAll()` **entre `initSDCard()` (`:1822`) e `initDVandAudio()` (`:1857`)**:

```c
// FrensHelpers.h
extern void (*Frens_preDisplayHook)(void);   // nullptr por padrão

// FrensHelpers.cpp, dentro de initAll(), logo após o f_mount bem-sucedido:
if (Frens_preDisplayHook) {
    watchdog_update();
    Frens_preDisplayHook();
    watchdog_update();
}
```

Sem alteração de assinatura de `initAll()` ⇒ **zero impacto nos 9 emuladores** (cada um tem sua própria cópia de `pico_shared`).

O hook do bootloader:
1. Chama `sd_boot_ini_load("/boot.txt", &s_pre_ini, ...)` e guarda o resultado num `static` de arquivo.
2. `ili9341_set_panel(ini.st7789 ? DMG_PANEL_ST7789 : DMG_PANEL_ILI9341)`.
3. `ili9341_set_invert(ini.invert)`.

Depois, em `main.cpp:1573-1581`, **substituir** a segunda chamada a `sd_boot_ini_load()` pelo uso do struct já parseado (parse único, fonte única de verdade). A tela de erro `BOOT.TXT INVALID` passa a ser exibida no hook, o que é aceitável porque o display já está funcional naquele ponto.

**Ganho adicional de PSRAM no bootloader:** com `PSRAM_CS_PIN=47`, `initPsram()` (`:1788`) passa a succeeding e `maxRomSize` passa a ser `psramMemorySize` (8 MB) em vez do derivado da flash (`:1790-1810`). Os themes/screensaver deixam de ser convertidos em RAM (skip em `main.cpp:1703-1706`) — **mais SRAM livre para o framebuffer**.

### 2.4 Regras de precedência edefaults

| Cenário | Painel | Rotação | Inverter |
|:--|:--|:--|:--|
| `boot.txt` ausente | `DEFAULT_PANEL` (build) | 0 | 0 |
| `boot.txt` presente, sem as chaves | `DEFAULT_PANEL` (build) | 0 | 0 |
| Chaves presentes | valor do arquivo | valor do arquivo | valor do arquivo |
| Cartão ausente / `f_open` falha | `DEFAULT_PANEL` (build) | 0 | 0 |
| Chave com valor inválido | erro fatal `BOOT.TXT INVALID` (consistente com `GB_MODE`/`VOLUME`) | idem | idem |

> Decisão: **erro fatal em valor inválido**, e não fallback silencioso — é o padrão já established para `GB_MODE`/`VOLUME`/`THEME` no parser e evita o clássico "a configuração não teve efeito e o usuário não sabe por quê".

### 2.5 Contraste de chaves novas

| Chave | Tipo | Padrão | Faixa | Destino |
|:--|:--|:--|:--|:--|
| `ST7789` | bool | `DEFAULT_PANEL` | `0`/`1` | seletor de painel |
| `ROTATE` | enum | `0` | `0`/`1` | `MADCTL` |
| `INVERT` | bool | `0` | `0`/`1` | `INVON`/`INVOFF` |

Novos flags de proveniência `seen_st7789`, `seen_rotate`, `seen_invert` em `sd_boot_ini_t`, para o escritor saber o que materializar.

**Chaves de diagnóstico (OPCIONAL, fora do contrato de 3 variáveis):** `ST7789_XOFF` / `ST7789_YOFF`, padrão `0`. A auditoria de bordo do PicoGBC confirma que 0/0 é o correto para este tipo de módulo; elas existem **apenas** para caso o teste de hardware mostre deslocamento, sem exigir recompilar. **Se preferir estritamente 3 chaves, basta remover este bloco** — o padrão 0/0 permanece.

**Chave `MADCTL` (OPCIONAL, added 29/09/2026 após teste de hardware):** o primeiro build no Core2350B real mostrou a imagem **espelhada** com o MADCTL derivado de `ROTATE=0` (`0x28`). A geometria do ST7789 do módulo não bate com a do módulo do PicoGBC, e a única forma de corrigir sem recompilar é escrever o registro cru. `MADCTL=0xNN` (hex, com ou sem `0x`) é aplicado **verbatim** e tem precedência sobre `ROTATE`.

| Bit | Mask | Efeito |
| :--- | :--- | :--- |
| `MY` | `0x80` | espelha verticalmente |
| `MX` | `0x40` | espelha horizontalmente |
| `MV` | `0x20` | troca linha/coluna (a rotação de 90/270°) |
| `ML` | `0x10` | data latch |
| `BGR` | `0x08` | ordem de cor (0 = RGB, 1 = BGR) |
| `MH` | `0x04` | refresh horizontal |

Partindo do valor efetivo do log, somar `0x80` corrige inversão vertical, `0x40` corrige horizontal, `0xC0` corrige os dois (180°). `MV` e `BGR` devem ficar como estão, a menos que o tamanho ou as cores também estejam errados.

Ao contrário de `ST7789_XOFF/YOFF`, esta chave **só é gravada por `sd_boot_ini_save()` se o usuário a tiver escrito** (`seen_madctl`). Materializar o valor derivado congelaria o default de hoje como se fosse uma escolha, e o próximo update de firmware deixaria de valer. API nova no driver: `ili9341_set_madctl()` / `ili9341_get_madctl()`; `ili9341_set_madctl()` marca o valor como explícito, então nem `ili9341_apply_default_rotation()` nem um `ili9341_set_rotation()` posterior o sobrescrevem.

**Atualização obrigatória de `sd_boot_ini_save()`** (`.c:376`): no caminho "arquivo ausente", gravar também `ST7789=`, `ROTATE=`, `INVERT=` com os valores efetivos. Sem isso, um cartão novo perde as chaves assim que o bootloader materializa o `boot.txt`. No caminho "arquivo presente", nada muda — as chaves passam byte a byte.

### 2.5a Resultado do teste em hardware: espelho do ST7789 do Core2350B = `MX` ligado

**Medido com o calibrador na tela (29/09/2026).** O ST7789 do Core2350B precisa de `MX` (0x40) a mais que o ILI9341: `0x60` = `0x28` + `MX`, mesma orientação paisagem com colunas invertidas, que é exatamente o espelhamento relatado. Nenhum datasheet declara os bits de espelho — eles dependem de como o controlador é ligado ao vidro, então o único valor confiável é o medido.

**Só o eixo de espelho foi medido aqui.** BGR precisou de um ajuste posterior, em 2.5c.

| Painel | Paisagem (`ROTATE=0`) | 180° (`ROTATE=1`) | Retrato |
| :--- | :--- | :--- | :--- |
| **ILI9341** (PicoDMG) | `0x28` | `0xE8` | `0x08` |
| **ST7789** (Core2350B) | **`0x60`** | `0xA0` | `0x40` |

O `select_madctl()` do driver passou a ter uma tabela **por painel**. Os valores do ILI9341 não foram tocados: aquele build é a base de regressão.

### 2.5b `ROTATE=1` no ST7789 entregava o valor do ILI9341, não o do ST7789

**Achado ao portar a tabela para o `shared_menu` (29/09/2026).** A tabela de 180° usava `| 0xC0`:

```c
case 3: s_madctl = st7789 ? (ST7789_MADCTL_LANDSCAPE | 0xC0) : ...;
```

O problema é o mesmo do item 2.5c, no eixo oposto: `MX` já está ligado no valor de paisagem do ST7789, então `OR` não o alterna — apenas acrescenta `MY`, devolvendo o valor invertido do **ILI9341**, ou seja, a mesma imagem espelhada no eixo errado. `0x60 | 0xC0` = `0xE0`; o correto é `0xA0`.

O erro é invisível no ILI9341, cujo valor de paisagem `0x28` não tem `MY` nem `MX`, então `OR` e `XOR` coincidem. Por isso passou pelo build de regressão e só apareceu ao exercitar a rotação no painel novo.

**Correção**: `XOR 0xC0` em `select_madctl()` (`ili9341.c:108`) e no equivalente do `shared_menu` (`dmg_panel.c`, `madctl_for()`). `XOR` é o correto para os dois painéis, então a tabela por painel continua valendo sem casos especiais. O ILI9341 não muda: `0x28 ^ 0xC0 = 0xE8`, o mesmo valor de antes.

**Ainda não validado em hardware** — o critério 4 da tabela de aceite está marcado `[PENDENTE]` até `ROTATE=1` ser conferido no Core2350B.

### 2.5c O bit BGR nunca foi exercido: vermelho e azul trocados no gameplay

**Sintoma (29/09/2026)**: menu com cores perfeitas, mas em 1942 o oceano saía vermelho.

**Não era o software.** A paleta é RGB555 de verdade — `CC()` (`main.cpp:173`) empacota `RRRGGGBB` com R nos bits 10-14, e `rgb555_to_rgb565()` (`ili9341.c:169`) faz R→R, G→G, B→B. O azul do oceano `0x006D90` sai como `0x0352`, azulado correto. Ordem de byte também não era: gameplay (`line_packed`, alto antes do baixo) e menu empacotam igual.

**Causa raiz**: o bit BGR (0x08) do MADCTL. Os dois painéis divergem nele, e o ST7789 lê os dados como RGB com BGR limpo — o oposto do ILI9341 com o mesmo bit. Herdado em `1`, o ST7789 trocava vermelho por azul.

**Por que passou tanto tempo sem aparecer**: o calibrador compara *orientações*, e `0x28` e `0x68` diferem apenas em MX — todos os 16 candidatos que ele oferecia compartilhavam um único valor de BGR. O espelho foi medido; a ordem de cor nunca foi testada. Some a isso que a paleta Clean Light não tem vermelho nem azul saturados na lista de ROMs, então o menu ficava bonito com a imagem errada embaixo. Subir o volume a 0 exibe um `[ MUTE ]` em vermelho: é o teste de um segundo dentro do próprio menu.

**Correção**: BGR limpo no ST7789 — paisagem `0x68` → `0x60`, retrato `0x48` → `0x40`, e o inverso `0xA8` → `0xA0`. Confirmado por disassembly nas duas peças (`PicoNES.elf` e `pico-bootLoader.elf`): `0x60`/`0xA0` para ST7789, `0x28`/`0xE8` para ILI9341, este último inalterado.

Como o `MADCTL=` do `/boot.txt` tem precedência e o emulador também o lê, dá para confirmar **sem reflashar**: gravar `MADCTL=0x60` no cartão e reiniciar.

**Calibrador na tela.** Perseguir isso editando `/boot.txt` e reiniciando custa uma ida ao cartão por tentativa, e um `MADCTL` errado no arquivo faz o parser estrito descartar o resto do `boot.txt` — um erro de digitação numa chave não relacionada apaga silenciosamente uma orientação correta. Então: **segurar SELECT por 1 s no splash** abre a lista de 16 candidatos; UP/DOWN trocam o valor na hora, **A grava no `/boot.txt`**, **B cancela** sem gravar. Só a tela decide o que é salvo, porque só ela mostra o resultado.

**Assinatura de build na tela.** `DMG_BOOT_BUILD_ID` aparece no splash e na tela de erro (`build panel3-20260929`). Sem ela não dava para distinguir "firmware antigo" de "`boot.txt` diferente do que o usuário editou" — foi exatamente essa confusão que gastou duas rodadas de tentativa cega, já que as duas produzem a tela espelhada.


`shared_menu/dmg_menu.h`:

```c
/* Painel (runtime, de boot.txt) */
bool     dmg_is_st7789_mode(void);        /* ST7789=1 */
uint8_t  dmg_get_panel_rotation(void);    /* ROTATE: 0 | 1 */
bool     dmg_get_panel_invert(void);      /* INVERT: 0 | 1 */

/* ROM em PSRAM (F3) */
typedef enum {
    DMG_ROM_SRC_FLASH = 0,   /* caminho legado: grava na flash XIP */
    DMG_ROM_SRC_PSRAM = 1,   /* microSD -> PSRAM, nunca toca a flash */
} dmg_rom_src_t;

dmg_rom_src_t dmg_get_rom_src(void);      /* PSRAM se detectada, senão FLASH */
bool          dmg_is_psram_available(void);
void          dmg_set_rom_src(dmg_rom_src_t src);   /* override para A/B */
```

`dmg_init_boot_mode()` (`dmg_menu.c:115-176`) é estendido com o parse de `ST7789`, `ROTATE` e `INVERT`, seguindo exatamente o padrão de `GB_MODE` (`:148`) e `VOLUME` (`:157`) — inclusive a lista de 4 caminhos de fallback (`"0:/boot.txt"`, `"/boot.txt"`, `"boot.txt"`, `"0:boot.txt"`) e a regra de `s_boot_mode_read` só ser marcado em `FR_OK` (`:130`), que é a correção que matou o watchdog travado do §7 do `AGENTS.md`.

**Ganho do HUD:** `dmg_volume_hud_get_row()` devolve linhas RGB565 **pré-compostas em RAM** (`dmg_menu.c:583-589`), injetadas pelo blitter do Core 1. Como a composição acontece no Core 0 sobre um buffer RGB565 **agnóstico de painel**, **zero mudança** para ST7789. Nenhum dos 9 emuladores precisa tocar no HUD.

### 2.7 Camada de ROM em PSRAM — API comum

Para os 7 projetos com `pico_shared`, **uma única implementação** em `FrensHelpers.cpp` reutilizando `Frens::f_malloc` (que já roteia para lwmem/PSRAM, `:934-940`):

```c
// FrensHelpers.h
typedef struct {
    uint8_t *data;          /* ponteiro na PSRAM, ou no XIP da flash */
    size_t   size;
    uint32_t crc;
    bool     in_psram;      /* true => data aponta para 0x11000000+ */
} FrensRomImage_t;

FrensRomImage_t *Frens_loadRomImage(const char *path, bool swapbytes,
                                   uint32_t max_size, int crc_offset);
void            Frens_freeRomImage(FrensRomImage_t *img);
```

Regras internas:
- Se `!Frens::isPsramEnabled()` → devolve `nullptr` com `crc` zerado; o chamador cai no caminho legado de flash (**fallback automático**).
- Aloca via `Frens::f_malloc(size)` (→ lwmem → PSRAM).
- **Escrita via alias não-cacheado**: `memcpy((void *)(0x15000000 + (uintptr_t)img - 0x11000000), buf, n)` ou `xip_cache_clean_flush_range` após cada bloco, espelhando o teste de escrita de `SetupPsram.c:207`. Sem isso, escritas em endereço cacheado podem ficar invisíveis para a CPU.
- Leitura do SD em blocos de 4 KB com `watchdog_update()` por iteração.
- `compute_crc32_buffer()` + byteswap in-place, reaproveitando `flashromtoPsram()` (`:1174-1198`).

Para **PicoGBC e PicoLINX** (sem `pico_shared`): portar `drivers/pico_psram/` + `drivers/lwmem/` + `lwmem_opts_ppp.h` para dentro dos dois projetos, e expor a mesma assinatura `Frens_loadRomImage()` num `dmg_rom.c` local. Assim os 9 ficam com **uma API idêntica**.

### 2.8 Eliminação do header de cache de ROM

PicoMD, PicoNGP, PicoWS, PicoLINX e PicoGBC gravam hoje um header de 256 B com tamanho + CRC + 32 amostras (ex.: `PicoWS/src/main.cpp:396-415`, `PicoNGP/src/main.cpp:625-661`) para pular o re-carregamento quando a ROM é a mesma.

**No caminho PSRAM esse mecanismo é removido.** Justificativa: sem cache, não há header, não há setor de metadados, não há `memcmp` de 32 amostras, não há `static_assert` de 256 B (que existe só para evitar `panic()` do SDK em `PicoPCE/main.cpp`). E o ganho é maior:SD→PSRAM (~1,7 s) em vez de SD→flash (~1,7 s) + erase + program + verify (~2,3 s).

**Mantém-se** apenas a validação de integridade semântica que cada emulador já faz antes de executar (`md_byteswap_words` + `memcmp` de 4 KB no PicoMD `:609-615`; `md_is_valid_rom` no `:774`; `ws_rom_getHeader` no `:649`; `pce` header) — estas verificam *conteúdo*, não *procedência*, e são o que impede um SD corrompido de travar o core.

### 2.9 Política de fallback (inegociável)

O caminho de flash **permanece compilado e funcional** em todos os 9. A escolha é feita em runtime:

```
Frens::initPsram()  →  JEDEC 0x9F, exige kgd == 0x5D
                    ├─ sucesso  →  DMG_ROM_SRC_PSRAM
                    └─ falha    →  DMG_ROM_SRC_FLASH  (comportamento idêntico ao de hoje)
```

Consequência: se a PSRAM do Core 2350B for ausente, estiver mal soldada, ou for um módulo de 2 MB em vez de 8 MB, o console **ainda funciona**, apenas mais lento para trocar de jogo. Isso também dá um caminho de **A/B test** imediato: `-DPSRAM_ROM=0` força o caminho antigo.

---

## 3. Fase 1 — `pico-bootLoader` (Placa 16)

> Escopo declarado: **começar pelo bootloader**, como pedido. Os emuladores ficam para as Fases 2–4.

### 1.1 `pico_shared/BoardConfigs.cmake` — novo `HW_CONFIG 16`

Adicionar após a linha `:626`:

```cmake
elseif ( HW_CONFIG EQUAL 16 )
    # --------------------------------------------------------------------
    # Waveshare Core 2350B (RP2350B) + 8MB PSRAM + SPI TFT (ILI9341/ST7789)
    # Console portátil PicoDMG. Controles 100% por GPIO (GP2..GP9).
    # PSRAM CS = GPIO 47 (header waveshare_core2350b.h do SDK 2.3.1).
    # --------------------------------------------------------------------
    set(USE_ILI9341 1 CACHE BOOL "Use SPI TFT display (ILI9341 or ST7789)")
    set(DVICONFIG "dviConfig_PicoDVI" CACHE STRING
    "Unused when USE_ILI9341 is set")
    set(LED_GPIO_PIN "25" CACHE STRING "Select the GPIO pin for LED")
    # SD card on SPI1
    set(SD_CS "13" CACHE STRING "Specify the Chip Select GPIO pin for the SD card")
    set(SD_SCK "14" CACHE STRING "Specify de Clock GPIO pin for the SD card")
    set(SD_MOSI "15" CACHE STRING "Select the Master Out Slave In GPIO pin for the SD card")
    set(SD_MISO "12" CACHE STRING "Select the Master In Slave Out GPIO pin for the SD card")
    set(SD_SPI "spi1" CACHE STRING "Select the SPI bus for SD card")
    # SPI TFT on SPI0 (identical wiring to HW_CONFIG 15)
    set(ILI9341_SPI_INST "spi0" CACHE STRING "SPI peripheral for SPI TFT")
    set(ILI9341_PIN_CS "17" CACHE STRING "SPI TFT chip select GPIO")
    set(ILI9341_PIN_SCK "18" CACHE STRING "SPI TFT SPI clock GPIO")
    set(ILI9341_PIN_MOSI "19" CACHE STRING "SPI TFT SPI MOSI GPIO")
    set(ILI9341_PIN_DC "20" CACHE STRING "SPI TFT data/command GPIO")
    set(ILI9341_PIN_RST "21" CACHE STRING "SPI TFT reset GPIO")
    set(ILI9341_PIN_BL "22" CACHE STRING "SPI TFT backlight GPIO")
    # No external controllers: GPIO buttons GP2..GP9 only
    set(NES_CLK "-1" CACHE STRING "")
    set(NES_DATA "-1" CACHE STRING "")
    set(NES_LAT "-1" CACHE STRING "")
    set(NES_PIO "pio1" CACHE STRING "")
    set(NES_CLK_1 "-1" CACHE STRING "")
    set(NES_DATA_1 "-1" CACHE STRING "")
    set(NES_LAT_1 "-1" CACHE STRING "")
    set(NES_PIO_1 "pio1" CACHE STRING "")
    set(WII_SDA "-1" CACHE STRING "")
    set(WII_SCL "-1" CACHE STRING "")
    set(WIIPAD_I2C "i2c0" CACHE STRING "")
    set(UART_ENABLED 1 CACHE STRING "Enable UART output")
    set(USE_I2S_AUDIO 0 CACHE STRING "Enable I2S audio output")
    set(PICO_AUDIO_I2S_DATA_PIN -1 CACHE STRING "")
    set(PICO_AUDIO_I2S_CLOCK_PIN_BASE -1 CACHE STRING "")
    set(PICO_AUDIO_I2S_PIO 1 CACHE STRING "")
    set(PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED 0 CACHE STRING "")
    set(PICO_AUDIO_I2S_RESET_PIN -1 CACHE STRING "")
    set(ENABLE_PIO_USB 0 CACHE BOOL "Enable PIO USB support")
    set(PIO_USB_USE_PIO 2 CACHE STRING "")
    set(PIO_DP_PLUS_PIN -1 CACHE STRING "")
    # *** THE ONLY FUNCTIONAL DIFFERENCE vs HW_CONFIG 15 ***
    set(PSRAM_CS_PIN 47 CACHE STRING "Select the GPIO pin for PSRAM chip select")
endif ( )
```

**Adicionar também** logo após `include("pico_shared/BoardConfigs.cmake")` (`CMakeLists.txt:58`), ainda **antes** do `FORCE` da linha 61:

```cmake
# Placa 16 (Waveshare Core RP2350B) usa o header do SDK; a placa antiga
# (HW_CONFIG 15) usa o header RP2350A. Ambos têm 16MB de flash.
if (HW_CONFIG EQUAL 16)
    set(PICO_BOARD waveshare_core2350b CACHE STRING "Board type" FORCE)
elseif (HW_CONFIG EQUAL 15)
    set(PICO_BOARD waveshare_rp2350_plus_16mb CACHE STRING "Board type" FORCE)
else()
    set(PICO_BOARD waveshare_rp2350_plus_16mb CACHE STRING "Board type" FORCE)
endif()
message("PICO_BOARD travado: ${PICO_BOARD}  (HW_CONFIG=${HW_CONFIG})")
```

E trocar o `HW_CONFIG` padrão (`CMakeLists.txt:46-52`) para não forçar 15 quando `USE_ILI9341` — mantendo 15 como padrão para não quebrar o build atual:

```cmake
if(NOT HW_CONFIG)
    if(USE_ILI9341)
        set(HW_CONFIG 15 CACHE STRING "...")   # inalterado: retrocompatível
    else()
        set(HW_CONFIG 2 CACHE STRING "...")
    endif()
endif()
```
A Placa 16 é sempre passada explicitamente: `cmake -B build_core2350b -G Ninja -DUSE_ILI9341=1 -DHW_CONFIG=16`.

### 1.2 `CMakeLists.txt` — defaults e limpeza USB

| Linha | Ação |
|:--|:--|
| `58-62` | Substituir o `FORCE` único pelo bloco acima |
| `66` | `add_compile_options(-Os)` — **manter**. O comentário em `main.cpp:1534` documenta que `-Os` já causou um deadlock de DMA no pipeline DVI quando `std::lock_guard` saiu para a flash. No caminho SPI TFT o Core 1 não usa DVI, mas mudar isso é risco sem ganho. |
| `211` | `PSRAM_CS_PIN=${PSRAM_CS_PIN}` — já passa o valor; nada a fazer |
| `262`, `282-285`, `295`, `304` | **Remover** `tinyusb_pico_pio_usb`, `tinyusb_host`, `tinyusb_board`, `xinput_host`, `tusb_xinput`, `add_subdirectory(tusb_xinput xinput_host)`, **apenas quando `HW_CONFIG EQUAL 16` ou `15`** (ambos são consoles portáteis sem controles USB) |
| `73-79`, `88-94` | Manter `family_support.cmake` e o bloco de PIO USB (eles são inócuos com `ENABLE_PIO_USB=0`); remover `add_subdirectory` de `xinput_host` é o que importa |
| novo | `target_compile_definitions(... DEFAULT_PANEL_ST7789=${DEFAULT_PANEL_ST7789})` com `set(DEFAULT_PANEL_ST7789 0 CACHE STRING "Painel padrão quando boot.txt ausente: 0=ILI9341 1=ST7789")` |

Ganho: remover `xinput_host` + `tusb_xinput` libera SRAM e flash e satisfaz a instrução "ignorar qualquer coisa que use controles externos".

### 1.3 `main.cpp` — 4 chamadas a `tuh_task()`

`main.cpp:474`, `:511`, `:937` ficam dentro de `#if USE_ILI9341`. Envolver em `#if !USE_ILI9341` ou remover, já que `tuh_task()` só faz sentido com host USB ativo. **Remover** é mais limpo e alinhado à instrução do projeto.

### 1.4 `src/sd_boot_ini.h` / `.c` — as 3 chaves

`sd_boot_ini.h`:
- Adicionar ao bloco de documentação do topo:
  ```
   *   ST7789=1                       (seleciona o painel ST7789 em vez do ILI9341)
   *   ROTATE=0                       (0 = paisagem 0 graus, 1 = paisagem 180 graus)
   *   INVERT=0                       (1 = inverte as cores, para painéis IPS)
   *   ST7789_XOFF=0                  (OPCIONAL, diagnóstico: offset de coluna do GRAM)
   *   ST7789_YOFF=0                  (OPCIONAL, diagnóstico: offset de linha do GRAM)
  ```
- Campos em `sd_boot_ini_t`:
  ```c
  bool    st7789;        /* ST7789=0|1;  default = DEFAULT_PANEL_ST7789 */
  uint8_t rotate;        /* ROTATE=0|1;  default 0 (paisagem 0 graus) */
  bool    invert;        /* INVERT=0|1;  default 0 */
  uint8_t st7789_xoff;   /* default 0 */
  uint8_t st7789_yoff;   /* default 0 */
  bool    seen_st7789;
  bool    seen_rotate;
  bool    seen_invert;
  bool    seen_st7789_xoff;
  bool    seen_st7789_yoff;
  ```

`sd_boot_ini.c`:
- **Defaults** em `sd_boot_ini_load()` (junto de `:43-54`):
  ```c
  out->st7789      = DEFAULT_PANEL_ST7789 ? true : false;
  out->rotate      = 0;
  out->invert      = false;
  out->st7789_xoff = 0;
  out->st7789_yoff = 0;
  ```
- **Parse**: inserir os ramos `ST7789`, `ROTATE`, `INVERT` (e os opcionais) no `else if` encadeado que já existe (após `THEME`, antes do tratamento de `GB_MODE`/`VOLUME` em `:208-220`), com a mesma guarda de duplicidade e a mesma validação de valor único. `DEFAULT_PANEL_ST7789` precisa de um fallback no `.c` (`#ifndef DEFAULT_PANEL_ST7789 / #define ... 0 / #endif`) para não depender da ordem de inclusão.
- **`sd_boot_ini_save()`** (`:376`): no caminho "arquivo ausente", acrescentar `ST7789=`, `ROTATE=`, `INVERT=` com os valores efetivos. No caminho "arquivo presente", nada muda (as linhas são copiadas byte a byte).

### 1.5 `pico_shared/FrensHelpers.cpp` — hook pré-display

1. Adicionar `extern void (*Frens_preDisplayHook)(void);` no header, definido como `nullptr` no `.cpp`.
2. Em `initAll()`, entre a linha `:1822` (`initSDCard()`) e `:1857` (`initDVandAudio()`), invocar o hook com `watchdog_update()` antes e depois.
3. **`PicoPlusPsram.cpp:23-27`** já faz retry automático no GPIO 47 se o CS configurado falhar — manter. Com `PSRAM_CS_PIN=47` o retry vira redundante, mas é uma rede de segurança útil se o CS for mudado.
4. Verificar se `frens_link_as_bootloader()` (`CMakeLists.txt:309`) e `BootPartition.cmake` continuam corretos sem alteração — **confirmado: não requerem mudança** (16 MB em ambos).

### 1.6 `src/drivers/ili9341/ili9341.c` — ramo ST7789

Estratégia de mínima difusão: manter o nome do arquivo e do alvo (`ili9341`), mantendo todas as assinaturas e a estrutura de `flush`/`flush_rows`/GB_MODE intactas. Só a fase de **init** e o `MADCTL` ganham um `switch`.

```c
// ---- header ili9341.h ----
typedef enum { DMG_PANEL_ILI9341 = 0, DMG_PANEL_ST7789 = 1 } dmg_panel_t;
void        ili9341_set_panel(dmg_panel_t panel);
dmg_panel_t ili9341_get_panel(void);
void        ili9341_set_invert(bool on);
bool        ili9341_get_invert(void);
```

```c
// ---- ili9341.c ----
static dmg_panel_t s_panel = DMG_PANEL_ILI9341;
static bool        s_invert = false;
static uint8_t     s_xoff = 0, s_yoff = 0;
static uint8_t     s_madctl = 0x28;

void ili9341_set_panel(dmg_panel_t p)  { s_panel = p; }
dmg_panel_t ili9341_get_panel(void)     { return s_panel; }
void ili9341_set_invert(bool on)        { s_invert = on; }
bool ili9341_get_invert(void)           { return s_invert; }

static void apply_invert(void) {
    write_cmd(s_invert ? CMD_INVON : CMD_INVOFF);
}

static void apply_madctl(void) {
    write_cmd(CMD_MADCTL);
    write_data(s_madctl);
}

/* called from ili9341_init(), after the hardware reset */
static void panel_init_sequence(void)
{
    switch (s_panel) {
    case DMG_PANEL_ST7789:
        write_cmd(CMD_SWRESET);   sleep_ms(150);
        write_cmd(CMD_SLPOUT);    sleep_ms(120);
        write_cmd(CMD_COLMOD);    write_data(0x55);
        apply_madctl();
        apply_invert();
        set_addr_window(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);
        write_cmd(CMD_NORON);     sleep_ms(10);
        write_cmd(CMD_DISPON);    sleep_ms(120);
        break;

    case DMG_PANEL_ILI9341:
    default:
        /* a sequência atual de ili9341.c:148-219, inalterada */
        ...
        apply_madctl();
        apply_invert();   /* NOVO: o ILI9341 atual nao emite 0x20/0x21 */
        break;
    }
}
```

`set_addr_window()` (`:97-114`) passa a somar `s_xoff` em X e `s_yoff` em Y:
```c
x0 += s_xoff; x1 += s_xoff; y0 += s_yoff; y1 += s_yoff;
```
Com o padrão 0/0 isso é um no-op ⇒ **zero risco** para o ILI9341 existente.

`ili9341_set_rotation()` (`:236-247`) mantém a tabela, mas grava em `s_madctl` e reaplica:
```c
void ili9341_set_rotation(uint8_t rotation)
{
    switch (rotation & 3) {
    case 0: s_madctl = 0x08; break;  /* retrato */
    case 1: s_madctl = 0x28; break;  /* paisagem (padrao) = ROTATE=0 */
    case 2: s_madctl = 0xC8; break;  /* retrato invertido */
    case 3: s_madctl = 0xE8; break;  /* paisagem invertida = ROTATE=1 */
    }
    apply_madctl();
}
```

Após `panel_init_sequence()`, em `ili9341_init()`: `spi_set_baudrate(62 MHz)`, `dma_claim_unused_channel(true)`, `memset(s_fb, 0, ...)`, e `xip_cache_inv_all()`.

**Reavaliar `spi_set_baudrate`:** 62 MHz é seguro para ILI9341 (validado no campo). Para ST7789, o teto típico é 62,5 MHz ⇒ mantém, mas deixar `DMG_PANEL_SPI_HZ` como `#define` ajustável para reavaliação rápida.

**Verificar:** `ili9341_flush()`/`ili9341_flush_rows()` **não precisam de mudança alguma** — CASET/PASET/RAMWR, o framing SPI de 16 bits, o DMA e as barras do GB_MODE são idênticos nos dois controladores.

### 1.7 `main.cpp` — hook e struct único

1. Declarar o hook **antes** de `Frens::initAll()` (linha `1535`):
   ```c
   static sd_boot_ini_t s_pre_ini;
   static bool s_pre_ini_valid = false;
   static char s_pre_ini_err[64];

   static void boot_pre_display_hook(void)
   {
       s_pre_ini_valid = false;
       if (sd_boot_ini_load("/boot.txt", &s_pre_ini, s_pre_ini_err, sizeof(s_pre_ini_err))
           != SD_BOOT_INI_OK) {
           /* nao fatal aqui: sem boot.txt, seguimos com os defaults de build */
           memset(&s_pre_ini, 0, sizeof(s_pre_ini));
           s_pre_ini.st7789 = (DEFAULT_PANEL_ST7789 != 0);
           s_pre_ini.rotate = 0;
           s_pre_ini.invert = 0;
           return;
       }
       s_pre_ini_valid = true;
   #if USE_ILI9341
       ili9341_set_panel(s_pre_ini.st7789 ? DMG_PANEL_ST7789 : DMG_PANEL_ILI9341);
       ili9341_set_invert(s_pre_ini.invert);
       ili9341_set_gram_offset(s_pre_ini.st7789_xoff, s_pre_ini.st7789_yoff);
   #endif
   }
   ```
2. Atribuir `Frens_preDisplayHook = boot_pre_display_hook;` imediatamente antes de `initAll()`; limpar para `nullptr` depois.
3. Em `1573-1581`, substituir a chamada direta por:
   ```c
   sd_boot_ini_t ini;
   char ini_err[64] = {0};
   if (s_pre_ini_valid) {
       ini = s_pre_ini;               /* ja parseado no hook */
   } else if (sd_boot_ini_load("/boot.txt", &ini, ini_err, sizeof(ini_err)) != SD_BOOT_INI_OK) {
       fatalErrorScreen("BOOT.TXT INVALID", ini_err, "Fix /boot.txt and reset.", nullptr);
   }
   ```
   Mantém a tela fatal para `boot.txt` presente mas malformado, sem parse duplicado.
4. Após `1587-1590`, aplicar a rotação:
   ```c
   #if USE_ILI9341
   ili9341_set_rotation(s_pre_ini_valid ? s_pre_ini.rotate : 0);
   ili9341_set_gb_mode(g_gb_mode);
   #endif
   ```
5. Ampliar o `LOG()` de `1582-1585` para incluir `ST7789`, `ROTATE`, `INVERT`.
6. **Nada** muda em: `scanDirForUf2()` (`:1011`), `flash_writer.c`, `uf2_loader.c`, `app_launch.c`, `crc32.cpp`, resolução de `g_emuDir` (`:1592-1601`), HUD de volume (`:1852-1920`) — exceto a largura do HUD, que em ST7789 permanece 134 px a `x=111` no modo GB e 160 px a `x=80` em tela cheia (é geometria de framebuffer, não de painel).

### 1.8 Script de build

Novo `build_core2350b.ps1`, espelhando `build_ili9341.ps1`:

```powershell
$env:PATH = "C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

cmake -B build_core2350b -G Ninja `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=16 `
  -DDEFAULT_PANEL_ST7789=1 `
  -DPICO_BOARD=waveshare_core2350b
ninja -C build_core2350b
```

Saída: `build_core2350b/pico-bootLoader.uf2`.

### 1.9 Critérios de aceite da Fase 1

| # | Teste | Resultado esperado |
|:--|:--|:--|
| 1 | `boot.txt` **ausente**, `-DDEFAULT_PANEL_ST7789=1` | Boot em ST7789, pré-visualização da Waveshare; theme/screensaver convertidos para PSRAM |
| 2 | `boot.txt` ausente, `-DDEFAULT_PANEL_ST7789=0` | Boot em ILI9341 (comportamento atual preservado) |
| 3 | `ST7789=1 ROTATE=0 INVERT=0` | Imagem correta, orientação 0° — **[OK]** orientação validada em hardware; **cores corrigidas depois** (2.5c), a validação original não as cobria |
| 3a | `boot.txt` **sem** `MADCTL=` | Splash mostra o MADCTL derivado do painel. **[OK]** — o default por painel dispensa a chave; era esse o objetivo do calibrador |
| 3b | `ST7789=1`, gameplay com cores saturadas | Vermelho e azul não trocados. **[PENDENTE]** `0x60` compila e está no ELF, mas ooceano de 1942 ainda não foi reconferido; ver 2.5c |
| 4 | `ST7789=1 ROTATE=1` | Imagem rotacionada 180° → `0xA0` (= `0x60` **XOR** `MY|MX`). **[PENDENTE]** a rotação responde ao `ROTATE`, mas o código entregava o valor do ILI9341; ver 2.5b |
| 4a | `MADCTL=` presente **e** `ROTATE=` | `MADCTL` tem precedência e `ROTATE` deixa de ter efeito. Comportamento proposital, mas contra-intuitivo na tela — motivo de o default agora já vir correto |
| 5 | `ST7789=1 INVERT=1` | Cores corrigidas |
| 6 | `ST7789=0` com ILI9341 físico | Idêntico ao comportamento atual (baseline de regressão) |
| 7 | `GB_MODE=1` + ST7789 | Janela 266x240 em `x=45..310`, barras pretas 45 px / 9 px |
| 8 | Log de boot | `PSRAM=1`; `boot_ini: ... ST7789=1 ROTATE=0 INVERT=0` |
| 9 | Flash 16 MB | `App partition size: 16252928 bytes` (15,5 MB) e `JEDEC` = 16777216 |
| 10 | Cartão SD ausente | Tela `SD CARD NOT FOUND` legível, com o painel padrão |
| 11 | SD removido após o boot | `f_mount` falha ⇒ fallback para flash, console **continua funcional** |
| 12 | Gravação de emulador | `Flashing...` + barra de progresso; `PicoNES.uf2` gravado em `0x10080000`; salto para o app |
| 13 | Botões | GP2..GP9 respondem UP/DOWN/LEFT/RIGHT/A/B/SELECT/START |
| 14 | `/emu/16/` | Emuladores em `0:/emu/16/` listados; fallback para `0:/emu/` quando ausente |
| 15 | `boot.txt` com `ST7789=2` | Tela `BOOT.TXT INVALID` com diagnóstico de linha |
| 16 | Retorno de emulador | `SELECT+START` → `scratch[7] = 0xB007BACE` → volta ao picker sem wipe da flash |

### 1.10 Riscos da Fase 1

| Risco | Impacto | Mitigação |
|:--|:--|:--|
| Offset de GRAM do ST7789 ≠ 0 | Imagem deslocada | Padrão 0/0 validado pelo PicoGBC; chaves `ST7789_XOFF/YOFF` como ajuste sem recompilar |
| Ordem de R/G/B no ST7789 | Cores trocadas (R↔B) | `MADCTL` bit `BGR`; se necessário, `ST7789_RGB_ORDER` como `-D` (referência: `st7789_remap_rgb565()` em `mk_st7789.c:172-209`) |
| Troca de bytes RGB565 | Imagem com ruído/coloração estranha | `ST7789_SWAP_RGB565_BYTES` como `-D` (mesma referência, `:74-75`) |
| Backlight ativo-baixo | Tela escura | Verificar `ILI9341_PIN_BL`; o driver assume ativo-alto em `:133`/`:233` |
| LED em GPIO 25 vs 39 | LED de status não acende | `LED_GPIO_PIN` na `HW_CONFIG 16`; confirmar a fiação |
| `PICO_SMPS_MODE_PIN` ausente no header | Boot instável se a placa usar SMPS | Confirmar se o Core2350B usa SMPS. `setClocksAndStartStdio(252000, 1.30V)` (`:1475`) é sensível a isso |
| Conflito de GPIO na nova placa | Display/SD inoperante | Auditar GPIO 2–9, 12–22, 47 contra o pinout do Core2350B. GPIO 47 é CS da PSRAM e não pode ser usado por mais nada |
| Remoção de `xinput_host` | Symbol undefined | Compilar e resolver o que sobrar referenciando TinyUSB |

---

## 4. Fase 2 — `shared_menu`

Baixo risco, sem hardware.

### 2.1 `dmg_menu.h` / `dmg_menu.c` — parser de painel

1. Adicionar a parse de `ST7789`, `ROTATE`, `INVERT` (e `ST7789_XOFF/YOFF` opcionais) em `dmg_init_boot_mode()` (`dmg_menu.c:115-176`), espelhando o padrão de `GB_MODE` (`:148`) e `VOLUME` (`:157`).
2. Novos statics: `s_st7789`, `s_rotate`, `s_invert`, `s_xoff`, `s_yoff`, `s_*_seen`.
3. Novas funções públicas conforme §2.6. Todas devem chamar `dmg_init_boot_mode()` primeiro, como `dmg_is_gb_mode()` (`:250-253`) e `dmg_get_global_volume()` (`:178-181`) já fazem.
4. **Cuidado com o tamanho do buffer:** `dmg_init_boot_mode()` lê 512 B (`:132-134`) e `dmg_save_global_volume_file()` usa `s_content[1024]`/`s_new_content[1024]` (`:189`, `:206`). Com 5 chaves a mais o arquivo cabe, mas vale confirmar que o `strncat` (`:223`, `:234-238`) não trunca. Ideal: migrar essas 3 variáveis para `dmg_init_boot_mode()` em vez de duplicar a lógica de parse em dois lugares — reduz o custo de manutenção e elimina a divergência de caminhos de fallback (ver §R1 em 4.1).

### 2.2 API de ROM em PSRAM (§2.7)

`dmg_get_rom_src()` decide com base em `dmg_is_psram_available()` (que por sua vez consulta `Frens::isPsramEnabled()` quando o projeto tem `pico_shared`). Em PicoGBC e PicoLINX, expor a mesma informação via a cópia local do driver de PSRAM.

### 2.3 Ganho colateral de simplicidade

`shared_menu` **nunca** toca em flash nem em XIP (verificado: nenhuma ocorrência de `flash_range_erase`, `flash_range_program`, `XIP_BASE`, `0x10000000`). Portanto **nada muda** em `dmg_config_save()` (`:329-354`) nem em `dmg_save_global_volume_file()` (`:183-248`) quando o painel ou a origem da ROM mudam.

**Dívida técnica pré-existente a registrar (não bloqueia):** `dmg_config_save()` e `dmg_save_global_volume_file()` **não chamam `watchdog_update()`**. Com cartão lento isso pode dar trip no watchdog de 6 s. Vale uma correção de 1 linha em cada.

---

## 5. Fase 3 — Emuladores: ROM em PSRAM

> A frente de **maior risco**. Executar em ordem crescente de dificuldade, com medição de FPS antes/depois em cada um.

### 5.0 Pré-requisitos comuns aos 9

1. `PICO_BOARD` → `waveshare_core2350b` (`FORCE`).
2. `HW_CONFIG=16` (mantém os pinos em um único lugar, agora idêntico ao do bootloader).
3. `PSRAM_CS_PIN=47` (e garantir que a linha `:630` do `BoardConfigs.cmake` da cópia local do emulador não force `0`).
4. **Não** linkar o `pico_psram` do SDK junto do `pico_psram` próprio — ambos reivindicam XIP CS1 e `0x11000000`. **Não** definir `PICO_PSRAM_SIZE_BYTES`: a região é gerenciada pelo lwmem.
5. Preservar `PICO_FLASH_SIZE_BYTES = 0x1000000` e o offset `0x10080000` — o bootloader valida isso (`WRONG_LINK_ADDR` em `main.cpp:1298`).
6. Manter `apply_safe_qmi_timing()` + `watchdog_update()` nos loops de SD (mesmo sem gravação de flash, ROM de 7 MB leva ~3 s).
7. `CLK_FAST_DEFAULT` do SD: manter 20 MHz (`AGENTS.md` §7 documenta 50 MHz instável). Para acelerar a leitura da ROM para a PSRAM, elevar para **32 MHz** é o candidato óbvio a testar — medir antes/depois.

### 5.1 Ordem de execução

| Ordem | Emulador | Complexidade | Motivo |
|:--:|:--|:--|:--|
| 1 | **PicoPCE** | Baixa | **Já tem** ramos `isPsramEnabled()` (`main.cpp:1400,1768-1770,1790,1846`) e `Frens::flashromtoPsram()` funcional. Serve de **prova de conceito** do caminho compartilhado. |
| 2 | **PicoNES** | Baixa | Usa `Frens::flashrom`/`flashromtoPsram` (`:1061`,`:1275`); só trocar o `ROM_FILE_ADDR` efetivo. |
| 3 | **PicoSMS/GG** | Média | `flashrom` local (`main.cpp:579`) que só precisa bifurcar no destino. |
| 4 | **PicoA26** | Média | `load_cart_rom_file` (`:379`) simples, `on_load_rom` já existe. |
| 5 | **PicoMD** | Média | Precisa remover o header de cache e converter `MD_ROM_CACHE_ABS` em ponteiro. Atenção ao skip do cabeçalho SMD de 512 B (`:591-595`). |
| 6 | **PicoNGP** | Média-alta | `rom.data` é `uint8_t*` **gravável** (`src/main.cpp:1024`); remover header de cache + 32 amostras. |
| 7 | **PicoWS** | **Alta** | `ws_init((uint8_t*)...)` (`:648`) grava no vetor; remover header de cache; SRG do V30 está em SRAM por histórico de PSRAM removida. Reavaliar se `ws_staticRam[64K]` + `externalEeprom[8K]` devem voltar para a PSRAM. |
| 8 | **PicoLINX** | Alta | **Não tem `pico_shared`** ⇒ portar `pico_psram` + `lwmem`. Além disso o alvo da flash é pós-`__flash_binary_end` (`:25`), layout não fixo. |
| 9 | **PicoGBC/DMG** | **Alta** | **Não tem `pico_shared`** ⇒ portar `pico_psram` + `lwmem`. Zero-copy `const uint8_t *` (`:252`) e alta pressão de SRAM (blitter DMA + áudio PIO). |

### 5.2 Padrão de alteração por emulador (Padrão B, o mais comum)

```c
// antes
static bool load_cart_rom_file(const char *filename)
{
    apply_safe_qmi_timing();
    ... constroi caminho ...
    if (f_open(&fil, path, FA_READ) != FR_OK) return false;
    f_size(&fil, &fsz);
    if (fsz < 512 || fsz > X_ROM_CACHE_MAX) { f_close(&fil); return false; }
    ... header + 32 amostras + cache hit ...
    for (...) { erase; program; memcmp; }      /* ~2.3 s de flash */
    apply_safe_qmi_timing();
    s_last_flashed_size = size;
    f_close(&fil);
    return true;
}

// depois
static bool load_cart_rom_file(const char *filename)
{
    apply_safe_qmi_timing();
    ... constroi caminho via dmg_get_current_rom_dir() ...

    if (dmg_get_rom_src() == DMG_ROM_SRC_PSRAM) {
        FrensRomImage_t *img =
            Frens_loadRomImage(path, false, X_PSRAM_ROM_MAX, X_CRC_OFFSET);
        if (!img) { /* cai no flash */ }
        else {
            s_rom = img->data;              /* 0x11000000 + offset */
            s_rom_size = img->size;
            s_rom_in_psram = true;
            x_cache_rom_if_writable();      /* copia para SRAM so o que o core grava */
            f_close / return true;
        }
    }
    ... caminho de flash legado, INTACTO ...
}
```

Padrão para o **PicoNGP**, onde o core grava no vetor:
```c
rom.data = s_rom;                 /* PSRAM: somente leitura para o core */
if (rom_needs_writable) {
    static uint8_t *rw;            /* buffer na SRAM interna */
    rw = malloc(s_rom_size);
    memcpy(rw, s_rom, s_rom_size);
    rom.data = rw;
}
```
Em PSRAM, `rom.data` deve ser `const uint8_t *` sempre que possível. SóNED/WRITE (mapperes de save, CHR banking) demandam buffer gravável em SRAM.

### 5.3 Ganho de SRAM como efeito colateral

Ao mover a ROM para fora da flash, os emuladores sob pressão (PicoWS 92%, PicoNGP) recuperam o espaço que antes era reservado ao headroom de `*_ROM_CACHE_HEADROOM` (1 MB em MD/NGP/WS) e dos buffers de página de programação. **Não aumentar o headroom de SRAM** — o oposto.

### 5.4 Subir o teto de tamanho de ROM

Com 8 MB de PSRAM, `*_ROM_CACHE_MAX` (hoje 4 MB) pode ir a **7 MB**, deixando ~1 MB de folga para lwmem/saves.Afeta MD, NGP, WS, LYNX. Comentar o limite com a UX: o selector passa a aceitar ROMs maiores, mas o `f_malloc` falhará de forma limpa se o lwmem não tiver espaço — `Frens::f_malloc` deve reportar e o loader deve voltar ao caminho de flash (ou recusar com mensagem clara), nunca fazer `memcpy` para `nullptr`.

### 5.5 Critérios de aceite da Fase 3

| # | Teste | Esperado |
|:--|:--|:--|
| 1 | Log de boot de cada emulador | `PSRAM=1`, `romsrc=PSRAM`, `psram_size=8388608` |
| 2 | Tempo de carga de ROM de 4 MB | de ~4,0 s para **~1,7 s** |
| 3 | ROM de 7 MB | carrega e roda |
| 4 | ROM de 8 MB (acima do teto) | recusa limpa com mensagem, sem crash |
| 5 | Troca de jogo 20x seguidas | sem vazamento de PSRAM (lwmem estável) |
| 6 | Retorno ao seletor e novo jogo | PSRAM liberada com `f_free` (modelo `PicoPCE/main.cpp:1768-1770`) |
| 7 | **FPS de gameplay** | **idêntico** ao pré-PSRAM, ou melhor. Se cair, investigar latência (§4.3) |
| 8 | Savestate salvar/carregar | idêntico |
| 9 | SRAM / cartucho | todo teste de bateria que passa hoje, passa |
| 10 | PSRAM ausente (JEDEC falha) | **fallback automático para flash**, console funcional |
| 11 | `-DPSRAM_ROM=0` | força o caminho legado, Useful para A/B |
| 12 | `SD` corrompido / ROM truncada | recusa limpa, sem `memcpy` para `nullptr` |

### 5.6 Riscos da Fase 3

| Risco | Emuladores | Mitigação |
|:--|:--|:--|
| **Latência XIP da PSRAM pior que a flash QSPI** — caches XIP compartilhados; PSRAM a 133/2 = ~126 MHz a 252 MHz de CPU vs QSPI a 90 MHz | MD, NGP, WS, PCE (acesso randômico pesado) | Medir FPS em cada. Mitigações: `__no_inline_not_in_flash_func` nos laços quentes (padrão já usado no projeto), cópia de janelas quentes (CHR do MD, PRG-RAM do NES, janela do HuCard) para SRAM, e `-DPSRAM_ROM=0` como saída |
| **Core grava no buffer da ROM** | NGP (`uint8_t*`), WS (`ws_init`), PCE (`LoadCard`) | Tornar `rom.data` `const` sempre que possível; buffer gravável em SRAM só quando estritamente necessário |
| **PicoGBC/PicoLINX sem `pico_shared`** | GBC/DMG, LYNX | Portar `pico_psram` + `lwmem` (cópia literal) e expor `Frens_loadRomImage()` idêntico |
| **Alta pressão de SRAM no GBC** | GBC/DMG | `PicoGBC/AGENTS.md` §14: blitter DMA em bands de 24 linhas + fila de 6 slots; a remoção do headroom de 1 MB de flash libera margem |
| **Regressão do histórico de PSRAM removida no WS** | WS | Reavaliar `ws_staticRam[64K]` + `externalEeprom[8K]` de volta para a PSRAM; medir |
| **`f_malloc` falhando** | todos | Checagem explícita de ponteiro nulo em todos os caminhos; nunca `memcpy` incondicional |
| **Fração de lwmem** | todos | `Frens::dumpHeapStats()` (`:1029-1055`) + `lwmem_get_stats_ex()` já existem — logar em cada troca de jogo |
| **Ressincronização do SD** | todos | Já documentado em `AGENTS.md` §7 para NGP; reaproveitar |

---

## 6. Fase 4 — Emuladores: driver ST7789

Feito **após** a Fase 3, para não misturar duas variáveis de risco no mesmo teste.

### 6.1 O que muda em cada driver

8 cópias de `ili9341.c` (PicoA26 19 KB, PicoPCE 29 KB, PicoSMS 34 KB, PicoMD 43 KB, PicoNGP 57 KB, PicoLINX 61 KB, PicoWS 70 KB) + `PicoGBC/src/firmware/mk_ili9341.c` (38 KB) e `PicoGBC/src/firmware/mk_st7789.c` (55 KB).

Aplicar **a mesma técnica da Fase 1 §1.6**: manter toda a estrutura de `flush`/`flush_rows`/blitter/GB_MODE e branchar apenas a fase de init e o `MADCTL`.

```c
bool dmg_is_st7789_mode(void);         /* de shared_menu, já pronto da Fase 2 */
uint8_t dmg_get_panel_rotation(void);
bool dmg_get_panel_invert(void);
```

```c
static void panel_init_sequence(void)
{
    if (dmg_is_st7789_mode()) {
        /* sequência de mk_st7789.c:592-617, já validada em hardware */
    } else {
        /* sequência ILI9341 atual, inalterada */
    }
}
```

`MADCTL` para landscape: `ROTATE=0` → `0x28`, `ROTATE=1` → `0xE8` (mesma tabela da Fase 1).
`INVON`/`INVOFF`: `INVERT=1` → `INVON`.

O `dmg_init_boot_mode()` (que precisa de FatFs montado) roda **antes** da init do display na maioria dos emuladores (verificado: `PicoA26 main.cpp:545`, `PicoLINX src/main.cpp:743`, `PicoWS src/main.cpp:936`, `PicoNGP src/main.cpp:1182`, `PicoPCE main.cpp:1643`, `PicoGBC src/firmware/main.c:2132`). **PicoMD, PicoSMS e PicoNES não chamam diretamente** — obtêm o valor de forma lazy por `dmg_config_load`/`dmg_rom_selector`/`dmg_ingame_menu`. Para esses 3, inserir uma chamada explícita a `dmg_init_boot_mode()` antes da init do display (o default já é lazy e retentivo, então é seguro).

Fallback quando o `boot.txt` não está acessível: `DEFAULT_PANEL_ST7789` por compilação, exatamente como no bootloader.

### 6.2 Casos especiais

| Emulador | Caso |
|:--|:--|
| **PicoGBC** | **Já tem os dois drivers.** `mk_st7789.c` está pronto e validado. Só ligar o seletor runtime sobre `mk_ili9225_init()` (`mk_st7789.c:619-642`). Definir `ST7789_X_OFFSET`/`ST7789_Y_OFFSET` (`:36-37`) a partir de `dmg_get_gram_offset()`. O `GB_RENDER_X` (`:44`) já usa `GB_RENDER_X_SHIFT = 18` e centraliza; verificar o interplay com `ST7789_X_OFFSET`. |
| **PicoLINX/NGP/WS** | Têm também um `drivers/st7789/st7789.c` **baseado em PIO** (~9 KB), usado só para a tela de boot/splash, **não** para o emulador. Deixar como está. O caminho do emulador é o `ili9341.c` SPI com framebuffer. |
| **PicoNGP** | Atenção ao bug já corrigido em `AGENTS.md` §8 ("Volume HUD Deslocado"): o blitter indexava coordenada absoluta num buffer de janela relativa. A emulação de ST7789 não deve alterar a janela ativa, mas o clamp `0 <= local_x < s_active_width` precisa ser preservado. |
| **PicoSMS/GG, PicoNES** | `is_rgb555 = true` ⇒ a paleta do `dmg_palette_t` e `dmg_volume_hud_render` (`:652`) usam RGB555. A conversão para RGB565 continua no driver; ST7789 consome RGB565, então **nenhuma mudança**. |
| **PicoPCE** | `pce_display_submenu` (submenu `Display >`): os modos > 266 px de largura escondem `Integer`. Geometria 320x240 é a mesma ⇒ regra inalterada. |

### 6.3 Critérios de aceite da Fase 4

| # | Teste | Esperado |
|:--|:--|:--|
| 1 | `ST7789=1` em cada um dos 9 | Boot, seletor de ROM, tela de loading, menu in-game e gameplay renderizam corretamente |
| 2 | `ROTATE=1` | Tudo rotacionado 180°, incluindo HUD de volume e overlay de FPS |
| 3 | `INVERT=1` | Cores corrigidas em todas as telas |
| 4 | `ST7789=0` com ILI9341 físico | **Idêntico** ao comportamento atual |
| 5 | `GB_MODE=1` + `ROTATE=1` | Janela 266x240 rotacionada, barras pretas corretas |
| 6 | HUD de volume | Aparece 1 s, centrado, **sem deslocamento**, some limpo (o caminho `dmg_volume_hud_get_row` é agnóstico de painel) |
| 7 | Overlay de FPS | Nas coordenadas corretas: `x = dmg_is_gb_mode() ? 53 : 8` (regressão de `AGENTS.md` §7) |
| 8 | Escala Fit/Stretch/Integer | Idêntica em ST7789 e ILI9341 |
| 9 | Boot sem `boot.txt` | Painel padrão de build |

### 6.4 Riscos da Fase 4

| Risco | Mitigação |
|:--|:--|
| Divergência de 9 cópias quase idênticas do driver | Extrair a fase de init para um header compartilhado em `shared_menu` (`dmg_panel_init.h`) que implemente só a sequência de registradores; cada `ili9341.c` mantém seu flush/blitter e chama o helper. **Reduz 9 implementações para 1.** |
| Larguras de SPI/DMA diferentes entre drivers | Nada a fazer — cada driver mantém seu `spi_init`/`baudrate`. Só a fase de init é unificada. |
| Incompatibilidade de `is_rgb555` | Nenhuma: RGB555 é interno, o painel recebe RGB565 |

---

## 7. Fases 5 e 6 — Validação e documentação

### 7.1 Matriz de regressão

Após cada emulador, rodar e anexar ao diário de bordo:

| Métrica | Como medir |
|:--|:--|
| Tempo de carga (SD→execução) | Cronômetro ou log com `get_absolute_time()` |
| FPS de gameplay | Overlay `START+A` (já existente) ou log periódico |
| SRAM livre | `Frens::dumpHeapStats()` / `lwmem_get_stats_ex()` |
| PSRAM livre | `PicoPlusPsram::GetAvailableBytes()` |
| watchdog | `watchdog_update()` em todo loop longo; zero resets inesperados |
| Boot limpo | 10 cold boots sem intervention |

### 7.2 Documentação a atualizar

| Arquivo | O que registrar |
|:--|:--|
| `AGENTS.md` (raiz) | Seção nova "Placa 16 — Core RP2350B + PSRAM + ST7789"; tabela das 3 chaves de `boot.txt`; mapa de memória (flash 16 MB + PSRAM 8 MB); migração ROM→PSRAM por emulador |
| `pico-bootLoader/AGENTS.md` | `HW_CONFIG 16`; pinos; sequência ST7789; `boot.txt` completo; `build_core2350b.ps1` |
| `shared_menu/AGENTS.md` (criar, se não existir) | Novas APIs de painel e de ROM |
| `Pico*/AGENTS.md` (9) | Tabela: padrão de loader, `*_ROM_CACHE_MAX` novo, remoção do header de cache, ganho de SRAM, resultado de FPS |

### 7.3 Script de build único (opcional, recomendado)

`build_all_core2350b.ps1` que itere os 9 emuladores com `-DPSRAM_ROM=1 -DHW_CONFIG=16 -DPICO_BOARD=waveshare_core2350b`, gera todos os `.uf2` e os copia para `C:\dev\PicoDMG-Temp\emu\16\` (o diretório que a Placa 16 lê), espelhando a estrutura de `buildAll.sh` existente.

---

## 8. Ordem de execução e Gates

```
GATE 0 ── pesquisa (CONCLUÍDO)
   │
   ▼
FASE 1 ── pico-bootLoader
   1.1  BoardConfigs.cmake: HW_CONFIG 16
   1.2  CMakeLists.txt: PICO_BOARD condicional + limpeza USB
   1.3  main.cpp: remover tuh_task()
   1.4  sd_boot_ini.{h,c}: ST7789 / ROTATE / INVERT
   1.5  FrensHelpers: Frens_preDisplayHook
   1.6  ili9341.{h,c}: ramo ST7789 + MADCTL por ROTATE + INVON/INVOFF
   1.7  main.cpp: hook + struct único
   1.8  build_core2350b.ps1
   │
   ▼
GATE 1 ── bootloader OK nos 16 critérios de 1.9
         PSRAM detectada (8 MB), boot.txt funcional, painel OK
         ► DECISÃO: seguir? Senão, iterar aqui.
   │
   ▼
FASE 2 ── shared_menu (sem hardware)
   2.1  parse de painel
   2.2  API de ROM em PSRAM
   2.3  (opcional) watchdog_update em dmg_config_save
   │
   ▼
GATE 2 ── shared_menu compila e a HW 15 não regrediu
   │
   ▼
FASE 3 ── ROM em PSRAM, em 9 lotes (5.1)
   3a  PicoPCE  (prova de conceito)
   3b  PicoNES
► 29/09/2026: PicoPCE HW16 + CHD entregue (porta dmg_panel, BoardConfigs HW16,
                     `ENABLE_CHD=1`, `build_core2350b.ps1`). HW15 sem regressão.
                     HuCard `.pce`/`.sgx` agora carrega na PSRAM via
                     `pce_load_rom_to_psram()` (espelho PicoNES, flag `s_rom_in_psram`);
                     buffer PSRAM liberado no ramo `isCDGame` antes de `LoadDisc`. CD/CHD
                     exige BIOS (`cd_find_bios()`: pasta do CUE/CHD → `/bios/`).
                     Ver `AGENTS.md` §7.1 "PicoPCE (2º emulador migrado ...)" e
                     `PicoPCE/agents.md` Diagnóstico 12.
                     ► Patch 13: stack do Core 0 relocada de SCRATCH_Y (3 KB) para
                     STACK em RAM (12 KB, `0x2007D000..0x20080000`) via
                     `ld_stack_override/` (`pico_add_linker_script_override_path`),
                     `PICO_STACK_SIZE=12288` + `PICO_CORE1_STACK_SIZE=3072` gated por
                     `USE_ILI9341`. Fix do congelamento em `.chd` (chd_read até ~10 KB —
                     estourava 4 KB de SCRATCH_Y). Ver `PicoPCE/agents.md` Diagnóstico 13.
                     3c  PicoSMS/GG
    3c  PicoSMS/GG
   3d  PicoA26
   3e  PicoMD
   3f  PicoNGP
   3g  PicoWS
   3h  PicoLINX
   3i  PicoGBC/DMG
   │
   ▼
GATE 3 ── todos os 9: carga < 2 s, FPS >= original, 0 regressões
   │
   ▼
FASE 4 ── ST7789 nos 9 (extraindo o init para shared_menu)
   │
   ▼
GATE 4 ── 9 emuladores OK com ST7789=1 / ROTATE / INVERT
   │
   ▼
FASE 5 ── documentação + build_all_core2350b.ps1
```

**Regra de parada obrigatória:** em nenhum Gate se avança com regressão aberta. A Placa 15 (HW_CONFIG 15, ILI9341) é o **baseline permanente** e deve continuar compilando e funcionando sem alterações — é a rede de segurança contra qualquer erro de pinout nesta migração.

---

## 9. Perguntas a responder antes de começar a Fase 3

Estas respostas evitam retrabalho na frente mais arriscada:

1. **`INVERT=1` significa "aplicar a correção"** (INVON) ou "o painel está invertido, não aplique"? Adotei a primeira (§2.2).
2. **Fiação dos botões** na Placa 16: continuam em **GP2..GP9**? (Assume-se que sim — o layout físico do console é o mesmo.)
3. **LED de status**: GPIO **25** (como na HW 15) ou **39** (padrão do header `core2350b`)?
4. **Fiação do microSD e do display SPI0** na Placa 16: idêntica (SD 12/13/14/15, TFT 17–22)? Precisa de **descritor/schematic** para confirmar.
5. **O Core2350B usa SMPS?** (`PICO_SMPS_MODE_PIN` não é definido no header.)
6. **Existe alguma outra coisa usando o GPIO 47** na placa? (é o CS da PSRAM)
7. **Voltar `ws_staticRam[64K]` e `externalEeprom[8K]` do PicoWS para a PSRAM** faz parte do escopo, ou fica para depois?
8. **`ST7789_XOFF` / `ST7789_YOFF`**: manter como chaves opcionais de diagnóstico, ou remover e usar só as 3 chaves pedidas?
9. **Teto de ROM**: subir para 7 MB agora, ou manter 4 MB e apenas migrar o destino?
10. **`CLK_FAST_DEFAULT` do SD**: subir de 20 MHz para 32 MHz como parte da otimização da carga, ou manter 20 MHz seguro?

---

## 10. Anexo — Referências de código verificadas

### Endereços e capacidades

| Item | Valor | Fonte |
|:--|:--|:--|
| Header da placa nova | `PICO_PSRAM_CS_PIN 47`, `PICO_AUTO_DETECT_PSRAM_SIZE 1` | `sdk/2.3.1/src/boards/include/boards/waveshare_core2350b.h` |
| Base da PSRAM | `0x11000000` | `pico_shared/drivers/pico_psram/PicoPlusPsram.cpp:13` |
| Alias não-cacheado | `0x15000000` | `SetupPsram.c:207` |
| PSRAM gravável via XIP | toda a janela SSI `0x10000000..0x1FFFFFFF` | `SetupPsram.c:204`; SDK `psram.c:284` |
| Deteção de 8 MB | `eid==0x26 \|\| size_id==2` | `SetupPsram.c:188-201` |
| Teto de clock da PSRAM | 133 MHz | `SetupPsram.c:140-163` |
| Retry automático no CS 47 | sim | `PicoPlusPsram.cpp:23-27` |
| Partição de apps | `0x10080000`, 15,5 MB | `pico_shared/BootPartition.cmake:32-38` |
| `PICO_FLASH_SIZE_BYTES` forçado | `0x1000000` (16 MB) | `BootPartition.cmake:103-104` |

### boot.txt / display

| Item | Referência |
|:--|:--|
| Parser do bootloader | `pico-bootLoader/src/sd_boot_ini.c:35` (load), `:376` (save) |
| `GB_MODE` / `VOLUME` existentes | `.c:48-49` (defaults), `.c:208-220` (parse) |
| `PICO_BOARD` forçado | `pico-bootLoader/CMakeLists.txt:61` |
| `HW_CONFIG` default 15 | `pico-bootLoader/CMakeLists.txt:46-52` |
| `PSRAM_CS_PIN 0` (HW 15) | `pico_shared/BoardConfigs.cmake:625` |
| `PSRAM_CS_PIN 47` (default global) | `pico_shared/BoardConfigs.cmake:675` |
| Init do ILI9341 | `pico-bootLoader/src/drivers/ili9341/ili9341.c:116-229` |
| Tabela de rotação | `ili9341.c:236-247` |
| `set_addr_window` | `ili9341.c:97-114` |
| Barras do GB_MODE | `ili9341.c:313-332`, `:369-388` |
| Sequência ST7789 validada | `PicoGBC/src/firmware/mk_st7789.c:592-617` |
| Offsets do GRAM ST7789 | `mk_st7789.c:36-37` (= 0) |
| `st7789_remap_rgb565` | `mk_st7789.c:172-209` |
| `initDVandAudio` (order) | `pico_shared/FrensHelpers.cpp:1706-1713` |
| `initPsram` | `FrensHelpers.cpp:1788` |
| `initSDCard` | `FrensHelpers.cpp:1822` |
| `tuh_task()` (a remover) | `main.cpp:474`, `:511`, `:937` |
| USB/XInput (a remover) | `CMakeLists.txt:262`, `:282-285`, `:295`, `:304` |

### shared_menu

| Item | Referência |
|:--|:--|
| `dmg_init_boot_mode` | `shared_menu/dmg_menu.c:115-176` |
| Parse `GB_MODE` / `VOLUME` | `dmg_menu.c:148` / `:157` |
| Fallbacks de caminho | `dmg_menu.c:119-128` |
| Regra `s_boot_mode_read` | `dmg_menu.c:130` |
| `dmg_config_save` | `dmg_menu.c:329-354` (sem `watchdog_update()`) |
| `dmg_save_global_volume_file` | `dmg_menu.c:183-248` |
| `dmg_volume_hud_get_row` | `dmg_menu.c:583-589` |
| Geometria GB | `dmg_menu.h:14-16` |
| `dmg_display_t` | `shared_menu/dmg_display.h:15-22` |
| Nenhum acesso a flash/XIP | verificado |

### ROM / PSRAM nos emuladores

| Item | Referência |
|:--|:--|
| `Frens::flashromtoPsram` (existe!) | `pico_shared/FrensHelpers.cpp:1061-1225` |
| `Frens::f_malloc` → lwmem/PSRAM | `FrensHelpers.cpp:934-940` |
| `Frens::f_free` / `f_realloc` | `FrensHelpers.cpp:965-975` / `:994-1000` |
| `Frens::dumpHeapStats` | `FrensHelpers.cpp:1029-1055` |
| `Frens::isPsramEnabled` | `FrensHelpers.cpp:1229` |
| Referência de free-on-exit | `PicoPCE/main.cpp:1768-1770` |
| Pattern de header de cache | `PicoWS/src/main.cpp:396-415`; `PicoNGP/src/main.cpp:625-661` |
| Skip de cabeçalho SMD 512 B | `PicoMD/main.cpp:591-595` |
| `rom.data` gravável (NGP) | `PicoNGP/src/main.cpp:1024` |
| `ws_init((uint8_t*)...)` | `PicoWS/src/main.cpp:648` |
| ROM zero-copy `const` (GBC) | `PicoGBC/src/firmware/main.c:252` |
| Alvo pós-binário (LYNX) | `PicoLINX/src/main.cpp:25-26` |
| Histórico de PSRAM removida (WS) | `PicoWS/AGENTS.md` §3 |
| SRAM 92% (WS) | `PicoWS/AGENTS.md` §3 |

---

## 11. Conclusão

O projeto tem uma **vantagem estrutural enorme** para esta migração: a infraestrutura de PSRAM (`SetupPsram.c`, `PicoPlusPsram.cpp`, lwmem) e a função `Frens::flashromtoPsram()` **já existem e estão completas**. O que falta é (a) trocar o `PICO_BOARD` para unlocking do GPIO 47, (b) corrigir `PSRAM_CS_PIN=0` → `47`, e (c) reescrever os 7 loaders que nunca chamam o caminho de PSRAM.

O display ST7789 é a parte **mais barata**, porque `MADCTL`, `COLMOD`, o framing SPI de 16 bits, o DMA e todo o mecanismo de GB_MODE são compartilhados entre os dois controladores — só a fase de init muda, e já existe uma implementação ST7789 validada em hardware neste mesmo projeto (`PicoGBC/mk_st7789.c`).

O ponto de atenção principal é a **latência de leitura da PSRAM versus a flash QSPI** em cores com acesso randômico pesado (Mega Drive, WonderSwan, NGP, PC Engine). Por isso o plano insiste em: (1) preservar o caminho de flash como fallback automático, (2) medir FPS por emulador antes e depois, e (3) oferecer um override `-DPSRAM_ROM=0` para A/B instantâneo.

Começar pela Fase 1 (bootloader) como combinado. Os 16 critérios de aceite do §1.9 são o gate para avançar.
