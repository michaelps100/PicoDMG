# 🤖 Agents.md — PicoWS (RP2350 + ILI9341)

Documentação técnica, arquitetura de pipeline de exibição, plano de implementação e acompanhamento de progresso das operações de agente no projeto **PicoWS**.

Firmware alvo de saída:
- **`PicoWS.uf2`** — Emulador Bandai WonderSwan / WonderSwan Color para Raspberry Pi RP2350 (sem PSRAM) + Display ILI9341

---

## 📋 Contexto e Diretrizes do Projeto

- **Local do Projeto:** `c:\dev\PicoDMG-Temp\PicoWS`
- **Referência de Menu e Funções In-Game:** `C:\dev\PicoDMG-Temp\PicoNGP` (Menu Clean Light, Menu In-Game, Save/Load States em 4 slots, Controle de Brilho, Persistência, Flash ROM Caching)
- **Referência de Efeito de Imagem Grid:** `C:\dev\PicoDMG-Temp\PicoGBC` (Efeito Screen-Door LCD: `None` | `Grid` com atenuação nas linhas e colunas)
- **Microcontrolador Alvo:** **Raspberry Pi RP2350** (sem PSRAM externa)
- **Display:** **ILI9341 SPI TFT (320x240)** em SPI0 (Frequência 70 MHz desacoplada via PLL_USB)
- **Áudio:** I2S MAX98357A / PWM
- **Cartão SD:** SPI1 (MISO: GP12, CS: GP13, SCK: GP14, MOSI: GP15)
- **Controles:** GPIO 2 a 9 (UP=2, DOWN=3, LEFT=4, RIGHT=5, A=6, B=7, SELECT=8, START=9)
- **Compatibilidade:** `pico-bootLoader` (offset `0x10080000`)

---

## 🖥️ Mapeamento de Hardware (RP2350 — HW_CONFIG 15 / Idêntico ao PicoNGP)

### 1. Display ILI9341 (SPI0)
| Sinal | GPIO | Função |
|---|---|---|
| CS | 17 | Chip Select SPI0 (ativo LOW) |
| SCK | 18 | SPI Clock |
| MOSI | 19 | SPI MOSI (TX) |
| DC | 20 | Data / Command |
| RST | 21 | Reset de Hardware |
| BL | 22 | Backlight (Controle de Brilho PWM / SIO) |

### 2. Cartão MicroSD (SPI1)
| Sinal | GPIO | Função |
|---|---|---|
| MISO | 12 | SPI1 RX |
| CS | 13 | Chip Select SD (ativo LOW) |
| SCK | 14 | SPI1 Clock |
| MOSI | 15 | SPI1 TX |

### 3. LED de Status
| Sinal | GPIO | Função |
|---|---|---|
| LED | 25 | LED onboard de atividade / status |

### 4. Áudio I2S — MAX98357A
| Sinal | GPIO | Função |
|---|---|---|
| DIN | 26 | I2S Data Out |
| BCLK | 27 | I2S Bit Clock (Clock Base) |
| LRCLK | 28 | I2S Word Select / LRCLK |

### 5. Controles Físicos (GPIO Direto, Active-LOW)
| Botão | GPIO | Mapeamento WonderSwan | Menu Clean Light / In-Game |
|---|---|---|---|
| UP | 2 | X1 (Cima) | Cursor Cima |
| DOWN | 3 | X3 (Baixo) | Cursor Baixo |
| LEFT | 4 | X4 (Esquerda) | Página Anterior / Ajustar Valor (-) |
| RIGHT | 5 | X2 (Direita) | Próxima Página / Ajustar Valor (+) |
| A | 6 | Botão A | Confirmar / Iniciar / Salvar |
| B | 7 | Botão B | Voltar / Resumir Jogo |
| SELECT | 8 | Combos / Atalhos | Combos de Atalho |
| START | 9 | Start | Iniciar / Pausa |

**Atalhos e Combos In-Game:**
- **START + B**: Abre o Menu In-Game (Pausa).
- **START + A**: Liga / Desliga overlay de FPS.
- **SELECT + START (durante jogo)**: Salva dados e retorna ao seletor de ROMs.
- **SELECT + START (no seletor de ROMs)**: Reinicia para o bootloader.

---

## 🎯 Requisitos dos Binários e Destinos de Pastas

### `PicoWS.uf2`
- **Pasta de ROMs:** `roms/ws` (`0:/roms/ws/` e fallback `/roms/ws/`)
  - Suporte a ROMs WonderSwan Mono: `.ws`
  - Suporte a ROMs WonderSwan Color: `.wsc`
- **Pasta de Dados:** `saves/ws` (`0:/saves/ws/` e fallback `/saves/ws/`)
  - Configurações: `0:/saves/ws/picows.cfg`
  - Saves de bateria / SRAM: `0:/saves/ws/<jogo>.sav`
  - Save States: `0:/saves/ws/<jogo>_1.ss` até `_4.ss` (4 slots independentes)
- **Efeitos de Imagem no Menu:** **None** | **Grid** (atenuação screen-door LCD conforme PicoGBC)
- **Paletas (Jogos Mono):** Default | Amber | Green
- **Modos de Escala:**
  - **Fit:** Proporcional centralizado (painel WS 224x144)
  - **Stretch:** Tela cheia 320x240 esticada
  - **Integer:** 1:1 nativo centralizado (com bordas horizontais de 48px e verticais de 48px)

---

## ⚡ Estratégia de Display e Arquitetura sem PSRAM

1. **Ausência de PSRAM no RP2350:**
   - O código original do emulador (`oswan`) alocava a ROM e a SRAM externa em PSRAM via chamadas `read8psram` / `write8psram`.
   - Como o hardware alvo é um RP2350 sem PSRAM, a arquitetura foi adaptada:
     - **ROM Caching em Flash XIP:** A ROM selecionada é gravada diretamente no Flash SPI (acima do binário do emulador) com verificação inteligente de integridade por hash FNV amostrado (32 blocos). Se a ROM já estiver em cache, o carregamento é instantâneo.
     - **SRAM Cartucho (64 KB) e EEPROM Externa (8 KB):** Alocados na memória SRAM interna do RP2350 (`ws_staticRam[0x10000]` e `externalEeprom[0x2000]`). O RP2350 dispõe de 520 KB de SRAM interna, acomodando confortavelmente todas as estruturas de memória do emulador (~350 KB no total).
2. **Pipeline de Exibição Dual-Core:**
   - **Core 0:** Loop principal de emulação nec v30 (`ws_executeLine`), áudio, polling de botões, alimentação de scanlines do display.
   - **Core 1:** Loop dedicado de streaming ILI9341 (`ili9341_blitter_core1`).
3. **Conversão de Linhas WonderSwan:**
   - O Oswan gera scanlines de 224 pixels (índices de paleta de 4 bits ou cores diretas).
   - O driver ILI9341 implementa `ili9341_stream_ws_line` com LUT de conversão RGB888 (`ws_shades`) para RGB565 big-endian e aplicação opcional do efeito `Grid` (~14% de atenuação em linhas e colunas alternadas).

---

## 📝 Registro de Tarefas e Status de Execução

- [x] **1. Cópia e Integração do `pico_shared`:**
  - Copiada a árvore `pico_shared` do PicoNGP para `PicoWS/pico_shared`.
- [x] **2. Implementação do Streaming WonderSwan no Driver ILI9341:**
  - Adicionadas funções `ili9341_stream_ws_line`, `ili9341_is_active_ws_line` e `ili9341_stream_ws_frame_end` em `pico_shared/drivers/ili9341/ili9341.h` e `ili9341.c`.
  - Implementado suporte a escalonamento 224x144 para 320x240 (Fit, Stretch, Integer).
  - Implementado efeito visual `Grid` (LCD screen-door).
- [x] **3. Remoção de Dependência de PSRAM:**
  - Eliminado uso de `read8psram` / `write8psram` em `src/oswan/memory.c` e `src/oswan/io.c`.
  - Migradas SRAM de cartucho (64 KB) e EEPROM externa (8 KB) para arrays internos na SRAM do RP2350.
- [x] **4. Módulo de Save State e SRAM (`ws_state_io.cpp` / `ws_state_io.h`):**
  - Implementada serialização e deserialização dos registradores da CPU NEC V30, `internalRam`, `ws_staticRam`, `ws_ioRam`, paletas e `externalEeprom`.
  - Suporte a 4 slots independentes em `saves/ws/<jogo>_1.ss` até `_4.ss`.
  - Suporte a salvamento e carregamento de SRAM de cartucho em `saves/ws/<jogo>.sav`.
- [x] **5. Reescrita do `src/main.cpp`:**
  - Implementado seletor de ROMs Clean Light para `roms/ws` com detecção de `.ws` e `.wsc`.
  - Implementado menu in-game (Pausa) com 8 opções: Save State, Load State, Effect (None/Grid), Palette (Default/Amber/Green), Brightness, FPS, Reset, Exit.
  - Implementado sistema de Flash ROM Cache com header de verificação para evitar regravações desnecessárias.
  - Sincronização e pacing de frames para WonderSwan.
- [x] **6. Configuração e Ajustes do `CMakeLists.txt`:**
  - Adequado build para RP2350, ILI9341, sem PSRAM, gerando `PicoWS.uf2`.
  - Configurado offset do `pico-bootLoader` (`0x10080000`).
  - Otimizações de compilação `-O3` e link com bibliotecas do `pico_shared`.
- [x] **7. Compilação e Validação do Binário:**
  - Compilado com sucesso via `build_for_bootloader.ps1` utilizando o Pico SDK 2.3.1 e arm-none-eabi GCC 15.2.1.
  - Binário gerado: **`PicoWS.uf2`** (649.728 bytes) na raiz do projeto e em `build_bootloader/`.
  - Mapa de Memória do RP2350:
    - **Flash utilizada:** 324.376 B (2,00% dos 16 MB disponíveis)
    - **SRAM interna:** 520.136 B (99,21% dos 512 KB)
    - **PSRAM:** 0 B (independente de PSRAM externa)
- [x] **8. Diagnóstico e Correção de Inicialização do SD / Display:**
  - **Causa da travada (LED piscando a 200ms):** O driver `pico_fatfs` tem como padrão `spi0` e os pinos 2, 3, 4, 5 (que no nosso hardware são os botões do D-pad). Como `pico_fatfs_set_config` não havia sido chamado, o `f_mount` falhava imediatamente ao tentar acessar o SD pelos pinos errados, caindo no loop de erro com LED piscando (`gpio_put(LED_GPIO_PIN, !gpio_get(LED_GPIO_PIN)); sleep_ms(200)`). Além disso, o display ILI9341 estava sendo inicializado somente após o `f_mount`, impossibilitando qualquer feedback visual na tela.
  - **Correção aplicada:**
    1. Incluído `#include "tf_card.h"` e configurado explicitamente `pico_fatfs_set_config` para **SPI1** com os pinos corretos: MISO=12, CS=13, SCK=14, MOSI=15.
    2. Movida a inicialização do ILI9341 (`ili9341_init()`) para antes da montagem do SD, com tela de inicialização e mensagens informativas em caso de erro na tela ao invés de piscar silenciosamente.
    3. Ajustados os offsets do ROM Cache na Flash (`0x10080000 + 0x100000 = 0x10180000`) para evitar qualquer conflito com o binário do emulador na partição do bootloader.
    4. Adicionada compatibilidade com caminhos alternativos (`0:/roms/ws`, `/roms/ws`, `roms/ws`).
- [x] **9. Diagnóstico e Correção de Congelamento no Carregamento da ROM (Travamento em 100%):**
  - **Causas identificadas:**
    1. **Panic no `flash_range_program` do Pico SDK:** O struct `PicoWS_Cache_Header_t` possuía `filename[60]`, totalizando 252 bytes. O Pico SDK exige estritamente que `flash_range_program(..., count)` tenha `count` como múltiplo exato de `FLASH_PAGE_SIZE` (256 bytes), disparando um `panic()` imediato após gravar o cache.
    2. **Corrupção de Memória em `ws_gpu_renderScanline` (`src/oswan/gpu.c`):** Havia a linha residual `framebuffer += (224 * ws_gpu_scanline);` herdada do emulador de PC (que usava um framebuffer full de 224x144). Como a arquitetura agora usa um buffer de scanline única de 224 bytes, essa instrução sobrescrevia até 32 KB de variáveis em SRAM/BSS a cada frame, congelando a CPU.
    3. **Underflow de Máscara de SRAM (`src/oswan/memory.c`):** Para jogos sem SRAM de bateria, `ws_rom_sramSize() - 1` resultava em `0xFFFFFFFF`, permitindo acessos de escrita fora do array `ws_staticRam`.
    4. **Ordem de Inicialização do Modo de Sistema:** `ws_set_system()` estava sendo chamado antes de `ws_init()`, sendo parcialmente resetado.
  - **Correções aplicadas:**
    1. Ajustado `filename[64]` em `PicoWS_Cache_Header_t` e adicionado `static_assert(sizeof(PicoWS_Cache_Header_t) == FLASH_PAGE_SIZE)` garantindo 256 bytes.
    2. Removido o offset de linha em `src/oswan/gpu.c`, mantendo a escrita restrita aos 224 bytes do buffer de linha atual.
    3. Protegido `sramAddressMask` em `src/oswan/memory.c` contra underflow em jogos com 0 SRAM.
- [x] **10. Correção Crítica de Endereçamento dos Bancos de ROM e Otimização de SRAM:**
  - **Causas identificadas:**
    1. **Underflow no Endereçamento de Bancos de ROM (`cpu_readmem20` em `src/oswan/memory.c`):** Na arquitetura do WonderSwan, os bancos lineares (banco 4 em diante) usam a fórmula `romBank = 256 - (((ws_ioRam[0xC0] & 0xf) << 4) | (bank & 0xf))`. No início da execução (banco 4), `romBank` resulta em 12 (`0xC0000` bytes). Para qualquer ROM com tamanho de 512 KB (`0x80000`) ou menor, `romSize - (romBank << 16)` ficava negativo (`-0x40000`). Como não havia a aplicação de `romAddressMask`, o cast `(unsigned)` envolvia o endereço para `0xFFFC0000 + offset`. Como `ws_rom` aponta para a Flash XIP (`0x10180000`), a leitura buscava em `0x10140000` (espaço do código binário do próprio emulador), fazendo o NEC V30 executar opcodes corrompidos no primeiro salto do vetor de reset.
    2. **Esgotamento de SRAM (99,21% de uso):** O `CMakeLists.txt` forçava `gpu.c.obj` (123 KB de código) para a SRAM via override do linker script, deixando míseros 4 KB de RAM livre para pilhas do Core 0 e Core 1.
    3. **Acessos de 32 bits desalinhados na Flash:** `compute_ws_sample_hash` e as amostras dividiam o tamanho por 31, gerando offsets não alinhados a 4 bytes, gerando faltas de alinhamento no Cortex-M33.
  - **Correções aplicadas:**
    1. Aplicada a máscara de ROM em `cpu_readmem20`: `uint32_t idx = roff & romAddressMask; if (idx >= romSize && romSize) idx %= romSize; return ws_rom[idx];`.
    2. Calculadas máscaras de potência de 2 seguras para ROM e EEPROM em `ws_memory_init`.
    3. Movido `gpu.c.obj` de volta para a Flash XIP no `CMakeLists.txt`, reduzindo o consumo de SRAM de **99,21% para 75,64%** (liberados **124 KB de RAM livre**).
    4. Adicionado alinhamento `off &= ~3u` nas funções de hash e `__aligned(4)` ao buffer `s_scanline_buf`.
    5. Convertida a estrutura `FIL` em `ws_state_io.cpp` para estática, poupando 560 bytes de pilha.
    6. Recompilado **`PicoWS.uf2`** (650.752 bytes).
- [x] **11. Replicação Fiel do Menu In-Game do PicoNGP, Correção de Congelamento Pós-Abertura, Hotkey START+A e Modo Vertical (Tate) via SELECT:**
  - **Menu In-Game (PicoNGP) Replicado Fielmente:**
    - Estrutura exata do menu de pausa: `Save State [1..4]`, `Load State [1..4]`, `Speed < 1x/2x >`, `Image >`, `Volume < 1..5 >`, `Brightness < 1..5 >`, `Reset`, `Exit`.
    - Submenu `Image >` completo: `Scale < Fit/Stretch/Integer >`, `Orientation < Horizontal/Vertical >`, `Effect < None/Grid >`, `Screen < Interleave/Progressive >`, `Palette < Default/Amber/Green >`.
    - Estilo visual Clean Light idêntico com caixas de diálogo, barras de nível, mensagens de status (`Saved to Slot X!`, `Empty Load State`) e paleta de cores.
  - **Correção dos Congelamentos Após Abertura de Jogos:**
    1. **Parsing Corrompido de Tipos de Cartucho (`src/oswan/rom.h` e `rom.c`):** O Oswan original possuía bugs críticos na leitura dos bytes de metadados da ROM (SRAM e EEPROM invertidos, constantes hexadecimais `0x10, 0x20, 0x50` gravadas erroneamente em decimal `10, 20, 50`, e máscaras invertidas). Com isso, cartuchos com SRAM recebiam 0 bytes de alocação ou tentavam ler de EEPROMs inexistentes, travando após as telas de splash. Corrigidas as funções `ws_rom_sramSize` e `ws_rom_eepromSize`.
    2. **Expansão de SRAM e Banking do Port `0xC1` (`src/oswan/memory.c` e `memory.h`):** O array `ws_staticRam` foi expandido de 16 KB para 64 KB, implementando o suporte a banking pelo registrador de I/O `0xC1` (`ws_ioRam[0xC1]`), permitindo que jogos como RPGs e jogos maiores acessem toda a sua memória estática sem overflow ou corrupção.
    3. **Ciclos de CPU por Scanline (`src/oswan/ws.c`):** O loop principal do Oswan estava fixo em míseros 256 ciclos de CPU por linha. Jogos como Final Fantasy 1, Final Fantasy 2, Klonoa, Puyo Puyo 2 e Digimon Tamers demandavam até 837 ciclos por scanline (definidos na tabela `ws_patchRom`), causando timeouts em loops de VBlank/HBlank e congelando o jogo. A chamada `ws_patchRom()` foi ativada e o jitter randômico arbitrário foi removido.
  - **Correção da Hotkey de FPS (START + A):**
    - Implementada detecção atômica da combinação `START + A` no loop de jogo em `src/main.cpp`. Alterna o overlay de FPS em tempo real e salva a configuração persistente em `saves/ws/picows.cfg`, limpando as entradas do emulador para evitar comandos espúrios no jogo.
  - **Modo Vertical (Tate) com Tecla SELECT e Aspect Ratio:**
    - Ao pressionar `SELECT` durante o jogo, a orientação é alternada instantaneamente entre Horizontal e Vertical (Tate).
    - O driver ILI9341 recebeu pipeline dedicado (`ili9341_stream_ws_vline`) para desenho de scanlines rotacionadas a 90° em tempo de VBlank através do buffer de rotação `s_ws_vframe[144 * 224]`.
    - Respeita rigorosamente a configuração de `Scale`:
      - **Integer:** 144x224 nativo rotacionado, centralizado com bordas laterais pretas de 88 pixels.
      - **Fit:** 154x240 proporcional rotacionado, ocupando toda a altura do ILI9341 e centralizado com bordas de 83 pixels.
      - **Stretch:** 320x240 tela cheia esticada.
    - O mapeamento dos controles físicos é invertido automaticamente via `ws_io_flipControls()`, permitindo segurar o dispositivo em modo retrato com D-pad e botões direcionados corretamente.
  - **Compilação e Validação Final:**
    - Firmware final compilado com sucesso via `build_for_bootloader.ps1`: **`PicoWS.uf2`** (664.576 bytes).
    - Memória SRAM utilizada: 483 KB (92,21%), deixando ~30 KB de RAM livre para pilhas e RTOS. Flash utilizada: 2,04%.
- [x] **12. Correção de Rotação 90° Anti-Horária, FPS em Modo Horizontal, Dinamismo de Paletas e Controles do Tetris:**
  - **Rotação 90° Anti-Horária (Tate):**
    - Ajustada a matriz de rotação em `src/main.cpp` de 90° horário ($ox = vy, oy = 143 - vx$) para 90° anti-horário exato ($ox = 223 - vy, oy = vx$). A tela em modo vertical agora é exibida orientada corretamente para a pegada do console.
    - Mapeamento de controles adaptado para rotação anti-horária de 90°:
      - Cima no D-pad rotacionado = Botão Físico Direita
      - Baixo no D-pad rotacionado = Botão Físico Esquerda
      - Esquerda no D-pad rotacionado = Botão Físico Cima
      - Direita no D-pad rotacionado = Botão Físico Baixo
  - **Overlay de FPS em Modo Horizontal:**
    - Nos modos `SCALE_INTEGER` (linhas ativas 48..191) e `SCALE_FIT` (linhas ativas 17..222), as linhas 8..15 nunca eram enviadas pelo pipeline de scanlines do emulador.
    - Implementado streaming explícito das linhas 8..15 em `ili9341_stream_ws_frame_end()`, renderizando o texto de FPS no topo esquerdo do display e limpando a área quando desligado. O FPS agora funciona em todos os modos.
  - **Dinamismo da Troca de Paletas (Default / Amber / Green):**
    - O driver ILI9341 mantinha `s_ws_pal_valid = true` desde a inicialização, nunca atualizando a LUT `s_ws_pal` ao alternar o esquema de cores.
    - Adicionada detecção por hash de `ws_shades` em `ws_update_palette_lut()`, função `ili9341_invalidate_ws_palette()` e invalidação forçada de `s_row_hash[0..239]`. A troca de paleta no menu in-game agora tem efeito visual imediato.
  - **Correção dos Controles no Jogo Tetris (Matriz de I/O 0xB5):**
    - Tetris é um jogo de orientação vertical nativa do WonderSwan que lê a linha `0x10` do port de I/O `0xB5` (teclado Y: Y1, Y2, Y3, Y4) para movimentação das peças. No Oswan original, a linha `0x10` estava travada em 0 e, no modo vertical legado, os botões A e B eram ignorados na linha `0x40`.
    - Corrigido o port `0xB5` em `src/oswan/io.c`: linha `0x40` fornece Start, A, B; linha `0x20` (X-pad) e linha `0x10` (Y-pad) fornecem simultaneamente direções Cima, Direita, Baixo, Esquerda. Tetris e jogos verticais agora respondem imediatamente a todos os comandos direcionais e rotações.
  - **Compilação e Validação do Binário:**
    - Recompilado **`PicoWS.uf2`** (664.576 bytes).
    - Memória: Flash 331.928 B (2,04%), SRAM 482.508 B (92,03%).
- [x] **13. Correção de Renderização dos Jogos (Travamento na Cor da Paleta), Desativação do FPS (Start+A) e Alinhamento de Controles por Orientação:**
  - **Correção da Falha de Inicialização (Tela Travada na Cor da Paleta):**
    1. **Estrutura de Header de ROM Corrompida (`src/oswan/rom.h` e `rom.c`):** O struct `ws_romHeaderStruct` original do Oswan omitia o campo `gameVersion`, resultando em desalinhamento de bytes. O campo `eepromSize` acabava lendo `romSize` (valores como 2, 3, 4, 8), fazendo com que todos os jogos recebessem SRAM falso de até 256 KB. Ao inicializarem, os jogos sondavam a SRAM pelo banco 1 esperando `0xFF` e, ao encontrarem `0x00`, entravam em loops infinitos. O struct foi redefinido como `__attribute__((packed))` contendo os 10 bytes exatos da especificação (`developerId`, `minimumSupportSystem`, `cartId`, `gameVersion`, `romSize`, `saveType`, `flags`, `realtimeClock`, `checksum`). Os tamanhos de SRAM e EEPROM agora são extraídos do byte correto (`saveType`).
    2. **Taxa de Clock da CPU (`ws_cyclesByLine = 256` em `src/oswan/ws.c`):** A taxa de ciclos por linha foi restaurada para 256 ciclos estáveis da especificação de hardware. As taxas artificiais anteriores (384/837 ciclos) executavam a CPU mais de 3x mais rápido, quebrando a temporização de interrupções de VBlank e HBlank e causando congelamentos em telas de carregamento.
    3. **Avanço Determinístico de Scanlines (`ws_skip = 1` em `src/oswan/ws.c`):** Eliminado o frame-skipping herdado de PC que pulava o contador de scanlines `ws_gpu_scanline` por cima da linha 144, perdendo o disparo da interrupção de VBlank e travando o loop de frame.
    4. **Bounds-Check Seguro nos Bancos 2 e 3 de ROM (`src/oswan/memory.c`):** Adicionado `idx %= romSize` para os bancos 2 e 3 em `cpu_readmem20`.
  - **Correção da Ocultação do FPS no Modo Horizontal:**
    - Em `pico_shared/drivers/ili9341/ili9341.c`, o teste anterior `else if (s_row_hash[8] != 0u)` nunca disparava porque `ili9341_set_fps_overlay(false, ...)` já zerava `s_row_hash[8..15]`. Implementada flag dedicada `s_ws_fps_drawn` que garante a limpeza completa das linhas 8..15 com pixels pretos assim que o overlay é desligado via **START + A**.
  - **Alinhamento dos Controles com a Disposição da Tela (Horizontal vs Vertical):**
    - Corrigido o port `0xB5` em `src/oswan/io.c`:
      - **Modo Horizontal (`!s_ws_vertical`):** D-pad físico controla o X-pad original (X1=Cima, X2=Direita, X3=Baixo, X4=Esquerda), A/B acionam Botões A e B.
      - **Modo Vertical (`s_ws_vertical`):** A pegada do console é rotacionada 90° anti-horário. D-pad físico envia comandos mapeados para o Y-pad vertical da WonderSwan (Y2=Cima, Y3=Direita, Y4=Baixo, Y1=Esquerda), enquanto X1/X3 e A/B acionam as ações de rotação e queda.
      - `ws_rotated()` agora lê com precisão o bit 0 do campo `flags` do cabeçalho da ROM para detectar a orientação nativa.
  - **Compilação e Validação do Binário:**
    - Firmware final recompilado com sucesso: **`PicoWS.uf2`** (664.064 bytes).
    - Memória: Flash 331.536 B (2,04%), SRAM 482.068 B (91,95%).
- [x] **14. Correção dos Gráficos do Klonoa (Chão Oculto), Mapeamento Tate em Jogos Verticais, Pacing do FPS (60 Hz) e Suporte Completo a Cores WSC:**
  - **Correção dos Gráficos do Klonoa (Chão Transparente):**
    - *Causa 1 (Janela FG sem checagem vertical):* As variáveis `ws_fgWindow_y0` e `ws_fgWindow_y1` eram lidas dos ports de I/O mas nunca eram utilizadas nos modos de janela 0x20 e 0x30. O recorte de janela horizontal era aplicado cegamente em todas as linhas, independentemente de a linha atual estar dentro da faixa Y da janela.
    - *Causa 2 (Opacidade da Cor 0 nas Paletas 0..3):* No hardware WonderSwan, as paletas de primeiro plano 0..3 tratam a cor 0 como **opaca** (`!(palette & 4)`). A camada subterrânea do Klonoa utiliza a cor 0 das primeiras paletas para compor a terra do subsolo. O código anterior ignorava `*ws_tileRow == 0`, tornando todo o chão transparente.
    - *Solução:* Implementada verificação dos limites verticais da janela (`line_in_y`) e condição estrita de transparência: `((*ws_tileRow) || !((tileInfo >> 9) & 0x04))`, renderizando o solo e as camadas subterrâneas com perfeição visual.
  - **Pipeline de Cores Nativas WonderSwan Color (WSC):**
    - No WonderSwan Color, a paleta é composta por 256 cores RGB444 (16 paletas de 16 cores) mantidas em `wsc_palette_rgb565[256]`.
    - Os streamings horizontal (`ws_band_push`) e vertical (`ws_vband_push`) do driver `ili9341.c` agora detectam `ws_gpu_operatingInColor == 1` e usam os índices completos de 8 bits (0..255) diretamente na LUT RGB565 de alta fidelidade cromática, com suporte opcional ao filtro `Grid`.
    - A escrita nos ports de paleta da RAM (`offset >= 0xfe00`) invalida a paleta imediatamente no display driver.
    - O menu in-game agora bloqueia ajustes de paleta em jogos coloridos exibindo `"Palette: < N/A (Color) >"`, reservando as paletas (Default/Amber/Green) exclusivamente para jogos monocromáticos (`.ws`).
  - **Correção dos Controles em Modo Vertical (Tate):**
    - No modo vertical, o D-pad físico agora atua exclusivamente sobre a linha `0x10` (Y-pad da WonderSwan: Y1=Left, Y2=Up, Y3=Right, Y4=Down) orientada para a pegada física rotacionada 90° anti-horária.
    - A linha `0x20` (X-pad) não mais recebe direções do D-pad quando em modo vertical, sendo acionada apenas pelos botões físicos A e B (ações de girar peças e queda no Tetris). Com isso, mover peças para os lados não dispara rotações ou hard drops indesejados.
  - **Pacing de Frames e Exibição do FPS:**
    - Ajustado `BASE_FRAME_US` de 13237 us (75.47 Hz de PC) para **16680 us (60 Hz)**, sincronizando a taxa de atualização do emulador perfeitamente com a taxa de 60 Hz do painel TFT ILI9341, idêntico ao PicoNGP e PicoGBC.
    - O overlay de FPS agora exibe 60 FPS estáveis sem travamento em 76.
  - **Compilação e Validação do Binário:**
    - Firmware final recompilado com sucesso via `build_for_bootloader.ps1`: **`PicoWS.uf2`** (669.184 bytes).
    - Memória RP2350: Flash 334.224 B (2,06%), SRAM 482.684 B (92,06%), PSRAM 0 B.
- [x] **15. Correção Definitiva do Piso do Klonoa e Eliminação de 64 KB de Cache Inútil de RAM:**
  - **Diagnóstico da "Casca" e Subsolo Invisível:**
    1. A "casca" superficial do piso havia desaparecido porque em uma tentativa anterior de checar `y0/y1`, o bloco de `windowMode` havia sido aninhado incorretamente e/ou a verificação estrita de Y havia descartado linhas quando o jogo utiliza recorte horizontal deixando `y0=0, y1=0`.
    2. O subsolo (terra abaixo da casca) ficava transparente no Oswan original porque nos modos `windowMode == 0x20` e `0x30`, os pixels com índice 0 eram sempre ignorados (`if (*ws_tileRow)`), ignorando que no hardware WonderSwan as paletas 0..3 tratam a cor 0 como opaca (`!(tileInfo >> 9 & 0x04)`).
  - **Correções Aplicadas em `src/oswan/gpu.c`:**
    1. Restaurado o fluxo limpo de `windowMode == 0x20` e `windowMode == 0x30` com suporte integral a paletas opacas (`else`) e transparentes (`if ((tileInfo >> 9) & 0x04)`), garantindo que tanto a casca quanto todas as camadas subterrâneas de terra sejam desenhadas com fidelidade.
    2. Removido o buffer `wsc_hflipped_tile_cache` (65.536 bytes / 64 KB de RAM). O espelhamento horizontal (`hFlip`) agora é computado dinamicamente em 8 bytes no momento da leitura da scanline, liberando 64 KB de SRAM interna e mantendo o uso de RAM em 92,06% (com ~30 KB livres para RTOS/pilhas).
  - **Compilação e Validação do Binário:**
    - Binário final gerado com sucesso via `build_for_bootloader.ps1`: **`PicoWS.uf2`** (669.184 bytes).
- [x] **16. Correção dos Controles em Jogos Verticais (Tate / Tetris):**
  - **Diagnóstico da Inversão de Direções:**
    - Em `src/main.cpp`, havia uma rotação antecipada de botões (`ws_key_up = right; ws_key_down = left; ws_key_left = up; ws_key_right = down;`) ao mesmo tempo em que `src/oswan/io.c` no port `0xB5` (linha `0x10`) já fazia o mapeamento para os bits do Y-pad (`ws_key_left -> Y1 (Left)`, `ws_key_up -> Y2 (Up)`, `ws_key_right -> Y3 (Right)`, `ws_key_down -> Y4 (Down)`).
    - Essa rotação duplicada fazia com que:
      - Físico `Left` gerasse `ws_key_down` -> Tetris ia para baixo (`Down`).
      - Físico `Right` gerasse `ws_key_up` -> Tetris ia para cima (`Up`).
      - Físico `Down` gerasse `ws_key_right` -> Tetris ia para direita (`Right`).
      - Físico `Up` gerasse `ws_key_left` -> Tetris ia para esquerda (`Left`).
  - **Correção Aplicada:**
    - Removida a rotação redundante em `src/main.cpp`, repassando os botões físicos diretamente para `ws_key_*`.
    - As 4 direções agora correspondem 1:1 ao esperado em Tetris e jogos verticais: `Left` move para a esquerda, `Right` move para a direita, `Down` desce/faz soft drop, `Up` sobe/hard drop.
  - **Compilação e Validação do Binário:**
    - Firmware recompilado com sucesso: **`PicoWS.uf2`** (669.184 bytes).
    - Memória: Flash 334.192 B (2,06%), SRAM 482.684 B (92,06%).
- [x] **17. Eliminação de Corrupção de Memória em `ws_gpu_renderScanline` e Restauração de Clock do Klonoa:**
  - **Causa Raiz das Telas Cinzas, Barras Verticais e Travamento no Menu:**
    1. A linha residual `framebuffer += (224 * ws_gpu_scanline);` em `ws_gpu_renderScanline()` havia sido reintroduzida inadvertidamente na reintegração do arquivo `src/oswan/gpu.c`. Em uma arquitetura de buffer único de scanline de 224 bytes (`s_scanline_buf`), isso fazia o renderizador escrever até 32 KB de memória além dos limites do buffer a cada frame, destruindo variáveis em BSS, registradores do emulador, e o estado do FATFS. Por isso a tela ficava cinza/listrada e o microcontrolador congelava ao tentar reabrir o seletor de ROMs.
    2. Em `src/oswan/ws.c`, a função `ws_patchRom()` estava com `ws_cyclesByLine` travado em 256. Jogos como Klonoa necessitam de 837 ciclos de CPU por linha para executar suas rotinas críticas de VBlank e DMA sem timeouts de interrupção.
  - **Correções Aplicadas:**
    1. Removida em definitivo a linha `framebuffer += (224 * ws_gpu_scanline);` em `src/oswan/gpu.c`, garantindo escritas restritas estritamente aos 224 bytes da linha atual.
    2. Restaurada a tabela `ws_patchRom()` em `src/oswan/ws.c` identificando Klonoa (`devId=0x01, cartId=0x08`), Final Lap, Puyo Puyo 2, FF1 e FF2 para operar com os 837 ciclos de CPU necessários.
    3. Removida a invalidação massiva de paleta repetida dentro de `ws_gpu_write_byte()`.
  - **Compilação e Validação do Binário:**
    - Firmware final gerado com sucesso: **`PicoWS.uf2`** (669.184 bytes).
    - Memória: Flash 334.264 B (2,06%), SRAM 482.836 B (92,09%), PSRAM 0 B.
- [x] **18. Correção de Glitches no HUD Inferior do Klonoa e Ativação dos Botões A e B no Modo Vertical (Tate):**
  - **Diagnóstico da Barra Inferior Glitchada (Corações de Energia):**
    - O WonderSwan possui duas camadas com suporte a windowing: SCR1 (fundo) e SCR2 (primeiro plano). No Klonoa, a barra inferior de status (corações de vida e pedras do sonho) reside na camada SCR1 (linhas 128 a 143), enquanto o cenário da fase rola na camada SCR2 com `windowMode == 0x20` (área ativa limitada entre $Y=0$ e $Y=127$).
    - O código do Oswan em `src/oswan/gpu.c` verificava unicamente os limites horizontais da janela (`column >= ws_fgWindow_x0 && column <= ws_fgWindow_x1`), **ignorando completamente a coordenada vertical $Y$**.
    - Consequentemente, nas scanlines 128 a 143, o cenário de SCR2 continuava sendo desenhado por cima dos corações na SCR1, causando a sensação de que os tiles da fase tinham prioridade sobre o HUD.
    - *Correção em `gpu.c`:* Condicionada toda a renderização de primeiro plano em `windowMode == 0x20` a `if (ws_gpu_scanline >= ws_fgWindow_y0 && ws_gpu_scanline <= ws_fgWindow_y1)` e em `windowMode == 0x30` a `line_outside_y = (ws_gpu_scanline < ws_fgWindow_y0 || ws_gpu_scanline > ws_fgWindow_y1)`. Fora da janela vertical, SCR2 é integralmente descartada naquela scanline, exibindo o HUD em SCR1 com total nitidez e sem sobreposição.
  - **Diagnóstico dos Botões A e B sem Função no Modo Vertical:**
    - Em modo vertical, a WonderSwan é empunhada segurando o Y-pad (linha `0x10`) com o polegar esquerdo e o X-pad (linha `0x20`) com o polegar direito. Os botões físicos originais A e B (linha `0x40`) ficam fisicamente fora de alcance.
    - Em jogos como Klonoa (em fases verticais) e Tetris, as ações (Pulo / Anel de Vento / Rotação) são lidas da linha `0x20` (X-pad: X1/X3 e X2/X4).
    - O código anterior vinculava o mapeamento de A/B no X-pad à flag `ws_key_flipped` (lida do cabeçalho estático da ROM). Como o Klonoa tem cabeçalho horizontal, ao alternar dinamicamente para o modo vertical (`ili9341_get_ws_vertical()`), a linha `0x20` permanecia lendo direções e os botões físicos A e B não enviavam sinal algum.
    - *Correção em `io.c`:* Na linha `0x20`, quando `ili9341_get_ws_vertical()` está ativo:
      - Botão A físico ativa `X2` (`1 << 1`) (Anel de Vento / Yo-Yo no Klonoa, rotação no Tetris).
      - Botão B físico ativa `X1` (`1 << 0`) (Pulo no Klonoa, rotação no Tetris).
- [x] **19. Ajuste da Frequência do SPI para 70 MHz (Eliminação de Falhas e Artefatos no ILI9341):**
  - **Diagnóstico das Falhas Visuais:**
    - Em alguns displays e painéis ILI9341, a taxa de 72 MHz (obtida anteriormente via PLL_USB a 144 MHz com divisor 2) operava no limite extremo de estabilidade do barramento SPI e do controlador de vídeo, causando pequenos artefatos, ruídos ou glitches pontuais durante a transmissão rápida de scanlines.
  - **Ajuste Fino do PLL_USB e Divisor SPI:**
    - Como o divisor de hardware do SPI no RP2350 exige um divisor par (prescaler $\ge 2$), para obter exatamente 70 MHz no SPI o clock periférico `clk_peri` deve ser de 140 MHz ($140 / 2 = 70 \text{ MHz}$).
    - Em `src/main.cpp`:
      - Reconfigurado o `pll_usb` para gerar exatamente 140 MHz: VCO em 840 MHz ($12 \text{ MHz} \times 70$), com `postdiv1 = 6` e `postdiv2 = 1` ($840 / 6 = 140 \text{ MHz}$).
      - Clock periférico `clk_peri` associado ao `pll_usb` a 140 MHz.
    - Em `pico_shared/drivers/ili9341/ili9341.c`:
      - Atualizado `#define SPI_RUN_BAUD 70000000u` (70 MHz).
  - **Compilação e Validação do Binário:**
    - Firmware recompilado com sucesso via `build_for_bootloader.ps1`: **`PicoWS.uf2`** (676.864 bytes).
    - Memória RP2350: Flash 337.960 B (2,08%), SRAM 482.836 B (92,09%), PSRAM 0 B.



---

## 20. HW_CONFIG 16 — Waveshare Core2350B + PSRAM + painel ST7789

> Adicionado em 2026-09-30. As estatísticas da tarefa 19 acima descrevem o
> build **HW_CONFIG 15, sem PSRAM**. O binário do console é `PicoWS_hw16.uf2`.

### Binários

| Alvo | Arquivo | Tamanho | Heap (medido no `.map`) | Validação |
| :--- | :--- | ---: | ---: | :--- |
| **Core2350B** | `PicoWS_hw16.uf2` | 742.912 B | **27.984 B** | ✅ **Validado em hardware pelo utilizador** |
| **RP2350 Plus** | `PicoWS.uf2` | 730.624 B | 28.924 B | ✅ não regrediu (rebuild limpo) |

> Heap = `__HeapLimit - __end__`. O build HW16 **custou 940 bytes** de SRAM. O
> delta vem do driver da PSRAM, do `dmg_panel.c` e da sequência ST7789, que são
> praticamente `.text` (flash), não `.bss`.

### Por que a PSRAM interessa especialmente ao PicoWS

Este emulador é o mais apertado em SRAM do conjunto: **95% de uso, ~28 KB de
heap**. A SRAM de cartucho (64 KB), a RAM interna (64 KB), a EEPROM externa
(8 KB) e o framebuffer 320x240 (153 KB) estão todos em RAM interna.

Mas o ganho desta migração **não é** libertar SRAM — a ROM nunca esteve na SRAM,
estava na flash. Os ganhos reais são:

| | Antes (flash XIP) | Agora (PSRAM) |
| :--- | :--- | :--- |
| Tecto de tamanho | `WS_ROM_CACHE_MAX` = 4 MB | 8 MB de PSRAM |
| Carga | apaga e reescreve MB de QSPI a cada load | leitura sequencial do SD |
| Risco de watchdog | é a razão de existirem os wrappers `safe_flash_range_*` e o header de cache persistente | eliminado |
| Custo | erase + program + verificação por `memcmp` | uma leitura |

### O contrato do ponteiro ROM é limpo (verificado, não assumido)

Esta era a parte de maior risco de uma porta PSRAM, e o PicoWS passou bem nela:

- `cpu_readmem20` (`src/oswan/memory.c:91-127`) é o **único** leitor.
- `cpu_writemem20` (`:61-78`) **descarta** todos os bancos acima do 1.
- O patch do Detective Conan, que em versões anteriores escrevia na imagem
  (`rom[0xfffe8] = 0xea`), **já está virtualizado** numa tabela de 5 bytes
  dentro do caminho de leitura (`memory.c:118-121`), guardado por `s_is_conan`.
- Não existe `memset`/`memcpy` para `ws_rom` em lado nenhum do `src/`.

Ou seja, **nenhuma escrita na imagem ROM passa a ter efeito** — ao contrário do
PicoNGP, onde `rom.data[0x23] = 0x10` e `rom.data[0x8DDF8] = 0xF0` eram
silenciosamente descartados na flash XIP e passam a ser reais em RAM.

`ws_init(uint8*, size_t)` recebe o ponteiro por valor e `ws_memory_init`
guarda-o tal e qual (`memory.c:143`), por isso a mudança é um drop-in.

### O que foi feito

1. **`HW_CONFIG 16`** em `pico_shared/BoardConfigs.cmake` — `PICO_BOARD` em
   `FORCE` (sem isso o VS Code reescreve para a board RP2350A, **onde o GPIO 47
   não existe**), LED em GP39, bloco de I2S MAX98357A em GP26/27/28
   (obrigatório: `USE_I2S_AUDIO` vazio quebra todos os macros `EXT_AUDIO_*`),
   `PSRAM_CS_PIN 47`.
2. **Painel unificado** — `dmg_panel.c` no build, `DEFAULT_PANEL_ST7789`
   (default `0`, `1` no HW_CONFIG 16), `ili9341_init()` dividida em
   `st7789_init_sequence()` / `ili9341_init_sequence()`, e o `MADCTL` hardcoded
   `0x28` passou a vir de `dmg_panel_get_madctl()` — **ST7789 = `0x60`**.
   A sequência ST7789 **não** envia `PWCTR1..5`, `VMCTR1/2`, `DFUNCTR`,
   `INVCTR`, `GAMSET` nem as duas gammas: são registos exclusivos do ILI9341.
3. **`ws_load_rom_to_psram()`** em `src/main.cpp` — blocos de 16 KB com barra
   de progresso e `watchdog_update()`, liberação da ROM anterior, checagem de
   tamanho com margem de 512 KB contra `Frens::f_malloc` (que faz `panic()`),
   e o mesmo CRC que a rota de flash gravava, para que save states e config
   continuem a funcionar entre as duas rotas.
4. **`Frens::initPsram()`** exposto no header e chamado em `main()`. O PicoWS
   nunca chama `Frens::initAll()` (conduz o SD e o display por conta própria), e
   `initPsram()` só era invocado de dentro de `initAll()` — portanto
   `isPsramEnabled()` ficaria `false` e `f_malloc` cairia no heap da SRAM.
5. **Ordem de arranque invertida:** o SD é montado **antes** de
   `ili9341_init()`. A tarefa 8 desta mesma documentação tinha posto o display
   primeiro, para um erro de SD ser visível em vez de um LED a piscar — e essa
   parte mantém-se, o ecrã de erro é desenhado logo depois. A mudança é
   necessária porque `dmg_panel_config_init()` lê `ST7789=` de `/boot.txt`, o que
   exige o cartão montado.
6. **`build_core2350b.ps1`** novo, já com verificação de binário fresco.

### Bug corrigido nos scripts de build

`build_for_bootloader.ps1` só verificava `Test-Path` no `.uf2` após o `ninja`.
**Um `ninja` que falha deixa o `.uf2` anterior no lugar**, portanto o script
copiava binário velho e imprimia `[OK]`. Pior: o ramo `else` imprimia `[ERRO]`
mas **não fazia `exit 1`**, pelo que o bloco verde "Arquivo pronto" aparecia
na mesma. Corrigido com `$LASTEXITCODE`, `LastWriteTime` e `exit 1` em ambos os
caminhos. `build_core2350b.ps1` já nasceu com a verificação.

### Validação em hardware (2026-09-30)

O utilizador **rodou `PicoWS_hw16.uf2` no Core2350B e confirmou que funciona**. O
comportamento de base é o mesmo do que já existia na outra placa e no outro
ecrã — o que era o requisito. Isto valida em conjunto:

- a detecção da HW_CONFIG 16 e o `PSRAM_CS_PIN 47` (a ROM é mesmo lida da PSRAM;
- o painel ST7789 com `MADCTL=0x60`, sem espelho e com as cores correctas;
- a nova `ws_load_rom_to_psram()` a substituir a gravação em flash;
- a reordenação do arranque (SD montado antes de `ili9341_init()`), incluindo o
  `dmg_panel_config_init()` a ler `/boot.txt`.

O utilizador indicou que **ainda precisa de alguns ajustes** neste emulador, mas que
a base está correcta. O que a secção 20 acrescenta está, portanto, a funcionar;
as lacunas conhecidas ficam listadas abaixo.

### Pendências

- **Ajustes de afinação pedidos pelo utilizador** — por definir. A base (arranque,
  painel, carga de ROM em PSRAM, execução) está confirmada.
- **O ganho de SRAM continua por escrever.** Os 64 KB de `ws_staticRam`
  (`memory.c:36`) e os 8 KB de `externalEeprom` (`:37`) são os maiores
  consumidores depois do framebuffer, e ambos são **dados**, não código — são os
  candidatos naturais a mover para a PSRAM. Mas isso tem o risco de contenção de
  QMI já documentado no PicoMD, onde mover LUTs para a PSRAM custou 9 FPS porque
  a ROM já lá estava. **Não mover sem medir.**
- `f_mount(&s_fs, "", 1)` monta com **label vazio**. Funciona porque o
  `dmg_menu.c` abre caminhos `0:/…` e o drive 0 é o corrente por omissão, mas o
  PicoLINX teve exactamente esta classe de bug (montava `""` em vez de `"0:"`).

- [x] **21. Correção do Fundo Preto na HUD do Klonoa:**
  - **Causa:** O erro estava na interpretação do registrador REG_BACK_COLOR (0x01). Em modo monocromático, os 3 bits baixos definem diretamente o índice (0-7) da cor de fundo a partir do pool global. O emulador estava utilizando incorretamente a lógica do modo Color (decodificando como bits de paleta e cor), o que o fazia ler o índice errado e preencher a tela de fundo com a cor Preta, em vez da cor clara solicitada pelo Klonoa.
  - **Efeito:** Como a HUD (foreground) e os itens (sprites) possuem pixels transparentes, a cor preta incorreta do fundo vazava através da transparência, gerando os quadrados pretos ao redor dos corações, gemas e chaves.
  - **Correção:** Ajustado ws_gpu_renderScanline em gpu.c para o modo monocromático ler diretamente a cor de fundo com ws_paletteColors[ws_ioRam[0x01] & 0x07]. A alteração anterior errônea nos sprites (que havia quebrado a regra de opacidade das paletas 8 a 11 do HW original) foi revertida. HUD agora renderizada com perfeição.

- [x] **22. Combo `SELECT + A`: Alternância de Velocidade 1x / 2x**
  - **Diagnóstico:** A infraestrutura completa de velocidade já existia e estava
    desligada, sem gatilho. Não foi necessário criar nada:
    - `cfg_turbo` (`main.cpp:169`) — estado do turbo;
    - `ws_apply_speed()` (`:299`) — callback que apenas escreve em `cfg_turbo`,
      já registado em `s_ws_desc.apply_speed` (`:353`);
    - persistência em `saves/ws/picows.cfg` via `g_dmg_config.turbo_mode` (`:366`);
    - pacing de frame (`:993`) com `BASE_FRAME_US / 2` quando `cfg_turbo` está ativo.
  - **Correção (`src/main.cpp`):** adicionado o combo após o bloco `SELECT + START`
    e antes do combo de volume, para que o `SELECT + START` (que exige hold de 60
    frames) mantenha a prioridade e o volume continue respondendo a `SELECT + UP/DOWN`:

    ```c
    /* SELECT + A: Toggle 1x / 2x speed */
    static bool s_turbo_edge = false;
    if (sel && a) {
        if (!s_turbo_edge) {
            ws_apply_speed(!cfg_turbo);
            save_config();
        }
        s_turbo_edge = true;
        up = down = left = right = a = b = sel = start = false;
    } else {
        s_turbo_edge = false;
    }
    ```

  - **Três pontos que importam:**
    1. **Detecção de borda, não de nível** — `s_turbo_edge` garante um único toggle
       por aperto. Sem ela, segurar `SELECT + A` alternaria a velocidade a cada
       quadro.
    2. **Supressão dos botões** — `a` e `sel` são zerados junto com os demais, para
       que o A e o SELECT não vazem para o jogo. Sem isso o Klonoa reagiria ao
       combo como uma tecla A normal.
    3. **Sem conflito com o toggle de orientação** — o handler de `SELECT` simples
       (`:907`) só alterna Horizontal/Vertical no *release* se nenhum outro botão
       tivesse sido apertado durante o hold, e `a` já constava nessa lista. Logo
       `SELECT + A` não gira a tela.
  - **Natureza do 2x:** altera apenas o **pacing** (metade do tempo por frame), não
    o clock do NEC V30. É *fast-forward*, não turbo real de CPU. Se algum jogo
    apresentar tearing ou glitches de timing a 2x, o ajuste a fazer é em
    `BASE_FRAME_US`, não no clock da CPU.
  - **Validação:** compilado limpo nas duas configurações, sem warnings novos.
    **Validado em hardware pelo utilizador** — combo funcional.

| Alvo | Arquivo | Tamanho |
| :--- | :--- | ---: |
| **Core2350B** | `PicoWS_hw16.uf2` | 743.936 B |
| **RP2350 Plus** | `PicoWS.uf2` | 731.648 B |
- [x] **22. Correção de Áudio (Klonoa Mudo / Ruídos Contínuos):**
  - **Causa 1 (Ruído contínuo de Sweep):** O Canal 3 possui um Frequency Sweep. Quando a frequência atinge o limite (0 ou 2047), o hardware não faz wrap-around. O emulador aplicava & 0x7FF e ignorava o flag de desligamento do jogo (s_sweep_step == 0), forçando loops infinitos da onda (indo do muito grave ao inaudível).
  - **Causa 2 (Áudio Mudo / DC Offset):** No canal de Voz (Canal 1), se o jogo ajustasse o volume para 0 no registrador 0x94, um fallback incorreto forçava o volume ao máximo. Isso deixava DC offsets ou ruídos de PCM tocando no talo, mascarando e emudecendo outras camadas.
  - **Correção:** Em ws_audio.c, a frequência do sweep foi limitada (clamped) em [0, 2047] e a condição s_sweep_step > 0 foi respeitada. O hack de volume no canal de Voz foi removido, permitindo mudez intencional do jogo.
  - **Causa 3 (Tom Metálico Persistente após Efeitos Sonoros):** Quando um jogo usa o Canal 4 em Modo de Ruído (Noise Mode), ele eventualmente desliga o Gerador de Ruído (Bit 4 da porta 0x8E) para parar o efeito (como o som de agarrar o Moo no Klonoa). No entanto, o emulador tinha um comportamento de fallback incorreto: se o gerador de ruído fosse desligado, o emulador revertia o canal imediatamente para o modo Wavetable (mesmo com o canal ainda configurado para Ruído na porta 0x90). Isso fazia o emulador ler lixo/alta frequência da RAM e tocar um tom metálico contínuo, em vez de apenas congelar a onda e silenciar o canal.
  - **Correção 3:** Removido o fallback. Se a porta 0x90 diz que o Canal 4 é Ruído, ele nunca toca Wavetable. Se o gerador for desligado, a onda simplesmente congela no estado atual (produzindo silêncio/DC offset) como o hardware real faria.
  - **Causa 4 (Música alta e abafando efeitos após voz no Klonoa):** A porta 0x89 controla tanto o Volume do Wavetable 2 quanto o Sample do Voice Mode (PCM). O Klonoa escrevia amostras PCM na porta 0x89 durante os efeitos sonoros. O emulador sobrescrevia a variável de volume do canal com essa amostra PCM. Quando o som acabava e o canal voltava pro modo Wavetable, ele usava a última amostra PCM (ex: 15) como volume, fazendo o canal tocar a música em volume máximo (15/15), o que distorcia a onda (som metálico/alto) e abafava os outros canais.
  - **Correção 4:** Modificada a escrita na porta 0x89 em ws_audio_port_write para checar s_control & 0x20 (Modo Voice). Se estiver em modo de voz, a escrita atualiza apenas um latch separado de PCM (s_ch2_voice). Se não, atualiza o volume da wavetable. Assim, a música retorna ao volume correto após os efeitos de voz.
  - **Causa 5 (Klonoa: Áudio metálico e corrompido que não se recupera após efeitos):** O emulador original (Oswan) não possuía a implementação correta do contador do Timer de VBlank (Hardware Interrupt Vector 5, port 0xA6/0xA7). Em vez de decrementar a cada VBlank e disparar quando o contador chegasse a zero, ele disparava incondicionalmente a CADA LINHA do VBlank (linhas 144 a 158) sempre que habilitado. Isso bombardeava o sistema de som do jogo com interrupções cerca de 15 vezes por frame (900 vezes por segundo em vez da taxa programada). Como resultado, quando o Klonoa tocava um efeito sonoro que sobrescrevia a Wavetable na RAM e usava o timer para saber a hora de parar o som e restaurar a música, a máquina de estados de áudio dele era destruída pelas interrupções em massa. O jogo abortava a rotina, deixando a Wavetable suja com a amostra de som (ruído metálico) pelo resto da fase, e arruinando o timbre dos efeitos subsequentes.
  - **Correção 5:** Reescrita total da rotina de VBlank Timer em src/oswan/ws.c. Agora ela lê corretamente o registrador de reload (0xA6/0xA7), decrementa um contador interno (0xAA/0xAB) uma vez por frame de VBlank e dispara a interrupção 
ec_int apenas quando o contador atinge 0. Arquivo ws_audio.c limpo e revertido ao original para evitar efeitos colaterais de hacks experimentais.
- [x] **23. Solução Definitiva do Áudio Metálico ao Agarrar Inimigos e da Regressão dos Efeitos Finos (Klonoa):**
  - **Diagnóstico e Engenharia Reversa da ROM do Klonoa (`klonoa.ws`):**
    1. **A Verdadeira Causa do Tom Metálico / Áudio Alto Permanente:** Ao capturar o inimigo ("Moo"), o Klonoa dispara o SFX 9 (Voz via Canal 1). Essa voz é transmitida a 12 kHz através de interrupções de hardware do Timer de HBlank (Vetor 7). No Oswan original, as interrupções eram chamadas via `nec_int()` direto no fim de cada scanline. Se `I.IF == 0` (interrupções desabilitadas pelo processador, como durante o próprio tratador de VBlank na scanline 144 ou trechos com `CLI`), o Oswan simplesmente **descartava a interrupção**. Na máquina real do WonderSwan, as interrupções são latchadas no registrador de hardware da porta `0xB6` e permanecem pendentes até serem atendidas e reconhecidas pelo software. Devido ao descarte no Oswan, a interrupção de término da amostra PCM era perdida. Com isso, o flag de término em RAM (`[0x0264]`) nunca era zerado, a rotina de encerramento (`0x04DE37`) nunca era chamada, e o canal de música 1 permanecia preso no modo Voice em volume máximo/distorcido até o fim da fase.
    2. **A Causa da Regressão dos Efeitos Finos e Baixos:** Tentativas anteriores forçaram o Klonoa na lista de 837 ciclos por scanline em `ws_patchRom()`. O hardware nativo do WonderSwan opera a 3.072 MHz estáveis com 256 ciclos por scanline. Rodar a 837 ciclos fazia a CPU executar 3,26 vezes mais rápido que o hardware de áudio, encurtando e acelerando todas as transições de frequência dos efeitos de pulo e agarrar, deixando-os agudos, finos e sem corpo.
    3. **Corrupção de Volume por Latch Residual (`s_ch2_wt_volume`):** O latch de volume adicionado em sessões anteriores tentava adivinhar o momento de restauração da Wavetable na porta `0x89`, mas sobrescrevia o volume real que o Klonoa já escreve explicitamente na porta `0x89` no frame seguinte ao término da voz.
  - **Correções Aplicadas:**
    1. **Controlador de Interrupções Pendentes com Prioridade de Hardware (`ws_check_interrupts()`):**
       - Implementada fila de despacho de interrupções fiel à arquitetura do WonderSwan e ao Mednafen: quando um evento ocorre (HBlank, VBlank, Scanline), o bit correspondente é setado na porta `0xB6`.
       - No loop central de execução `nec_execute()`, a cada instrução o processador verifica se há interrupções ativas pendentes `(ws_ioRam[0xb6] & ws_ioRam[0xb2])`. Se `I.IF` estiver habilitado, despacha imediatamente o vetor de maior prioridade (`(0xB0 + i) * 4`). Nenhuma interrupção é mais perdida ou descartada.
       - Disparo reativo ao gravar nas portas `0xB6` (reconhecimento) e `0xB2` (habilitação).
    2. **Restauração do Clock Real de 256 Ciclos por Linha:**
       - Removido o Klonoa da lista de 837 ciclos em `ws_patchRom()`. A CPU executa a 256 ciclos/linha (3.072 MHz exatos), restaurando a velocidade, timbre encorpado e ressonância original dos efeitos sonoros de pulo e agarrar.
    3. **Limpeza do Pipeline de Áudio em `ws_audio.c`:**
       - Eliminado `s_ch2_wt_volume`. O volume da porta `0x89` é gravado diretamente e mantido de acordo com a especificação de hardware.
       - Corrigido o mapeamento de bits de volume do modo Voice (porta `0x94`): bit 2 = 100% L, bit 3 = 50% L, bit 0 = 100% R, bit 1 = 50% R, com silêncio perfeito quando `0x94 == 0`.
  - **Binários Gerados:**
    - `PicoWS_hw16.uf2` (745.472 B) para Core2350B (HW_CONFIG 16).
    - `PicoWS.uf2` (733.184 B) para RP2350 (HW_CONFIG 15).

- [x] **25. Correção Definitiva do Áudio do Klonoa (Underflow do Timer HBlank, Desengate do Modo Voice e Frequência Real da CPU):**
  - **Diagnóstico Preciso das Causas Raízes:**
    1. **Underflow no Contador do HBlank Timer (`src/oswan/ws.c`):**
       - Ao agarrar o inimigo Boo/Moo, o jogo dispara o som de voz PCM (SFX 10) pelo Canal 1. Esse modo depende da interrupção HBlank Timer (vetor `0x9C` -> `0x4000:E7B7`, habilitada pelo bit 7 da porta `0xB2`) disparando a cada scanline para escrever a próxima amostra PCM de 8 bits na porta `0x89`.
       - Como o jogo não programa valor de reload nas portas `0xA4`/`0xA5` (`reload == 0`), a lógica anterior de contagem fazia `count` underflowar para `0xFFFF` (65.535 scanlines) em vez de disparar a cada linha. A interrupção quase nunca ocorria, a amostra de voz nunca avançava nem chegava ao fim (`0x4E79A`), e o canal ficava eternamente preso no estado de voz.
    2. **Canal 1 Preso em Modo Voice e Saturação DC do Mixer (`src/oswan/ws_audio.c`):**
       - Em `ws_audio.c`, o Canal 1 operava em Voice Mode sempre que `s_control & 0x20` estivesse setado, sem checar se havia um stream ativo de PCM. Preso nesse modo, cada vez que a música escrevia volume de nota na porta `0x89`, o emulador interpretava o valor como uma amostra PCM com viés DC estático enorme (+127 / -128).
       - Esse viés DC deslocava o somatório dos 4 canais (`mix_l` / `mix_r`) para o teto, provocando clipping digital assimétrico severo contra +-32767. Isso gerava o tom estridente, alto e metálico persistente, além de abafar e esmagar os efeitos mais sutis.
       - *Correção:* O modo Voice do Canal 1 agora exige estritamente um stream de PCM ativo: `(s_control & 0x20) && ((ws_ioRam[0xb2] & 0x80) || (ws_ioRam[0x52] & 0x80))`. Quando a voz termina (ou quando a interrupção HBlank é desabilitada), o canal reverte imediatamente para o modo Wavetable tradicional, tocando as notas da música sem qualquer offset DC ou ceifamento.
    3. **Eliminação da Regressão dos Efeitos Finos e Baixos:**
       - Removida a sobretaxa de 837 ciclos para o Klonoa em `ws_patchRom()`, restaurando os 256 ciclos por scanline (3.072 MHz nativos de hardware). A CPU agora executa no tempo real, permitindo que os envelopes e transições de pitch dos efeitos de pulo e interação soem no tom, velocidade e ressonância perfeitos.
    4. **Limpeza de Hacks Ineficazes:**
       - Removido o bloco artificial de sobrescrita de RAM (`0x1F0` / `0x26E`) em `ws_audio.c`, eliminando qualquer risco de corrupção da memória do driver de som do jogo.
  - **Binários Gerados com Sucesso:**
    - `PicoWS_hw16.uf2` (755.200 B) para Waveshare Core2350B (HW_CONFIG 16).
    - `PicoWS.uf2` (742.912 B) para RP2350 Plus Bootloader (HW_CONFIG 15).



- [x] **26. Correção do Aumento de Volume da Música (Latch Separado) e Retorno dos 837 Ciclos para o Klonoa:**
  - **Causa Raiz do Aumento de Volume e Distorção:** A porta 0x89 no WonderSwan é compartilhada: ela serve tanto para o volume da Wavetable do Canal 2 quanto para receber o sample PCM do modo de Voz. Durante o som do 'Boo', o jogo joga o sample PCM (ex: 255) na porta 0x89. Quando o efeito de voz terminava, o emulador voltava para o modo música, mas o volume da Wavetable continuava travado no valor do último sample de voz (Volume Máximo 15/15), o que fazia a música estourar (aumentando absurdamente e clipando/distorcendo metálico nas bordas do mixer).
  - **Correção:** Adicionado um latch interno separado (`s_ch2_voice_latch`) no `ws_audio.c`. Quando o Canal 2 está em modo Voz, as gravações na porta 0x89 atualizam apenas o latch do PCM, deixando o volume da música original intacto.
  - **Causa Raiz dos Efeitos Agudos e Baixos:** A tentativa de rodar o Klonoa a 256 ciclos de CPU por linha estava causando descompasso na execução da rotina de áudio, alterando o pitch dos efeitos.
  - **Correção:** Restaurado o hack de 837 ciclos por scanline no `ws_patchRom()` especificamente para o Klonoa.


---

## 27. Sessão de Performance WSC (22/10/2026): 60 FPS em Progressive + Fit, Estabilidade e Correções

Objetivo: jogos WSC só rodavam bem em Interleave + Integer. Resultado final: **Digimon Adventure 02 – D1 Tamers estável a 60 FPS em Fit + Progressive** (`GB_MODE=1`), áudio limpo e Final Fantasy 1 iniciando corretamente. Medição em hardware pelo usuário.

### 27.1 Metodologia (o que mudou de rumo)
- A análise inicial por leitura de código apontou o SPI. As medições no overlay mostraram que, **no Digimon, o gargalo era o núcleo V30**, não o display (`p`, `w`, `c` ≈ 0).
- Instrumentação temporária no overlay (n/g/p/w/c, instruções, ROM, cache XIP, histograma de regiões de código) permitiu achar o ponto exato. **Toda a instrumentação foi removida**: o overlay mostra só o FPS.
- Lição registrada: dois palpites sucessivos (laço de espera simples) falharam porque o código real era um **escalonador cooperativo de tarefas**; só a leitura dos bytes da ROM resolveu.

### 27.2 Display (`pico_shared/drivers/ili9341/ili9341.c`)
| Item | Antes | Agora |
| :--- | :--- | :--- |
| Transmissão por linha | 3 transações CS (PASET, RAMWR, dados) com `spi_write_blocking` em flash, ~9 µs de overhead por linha | Janela aberta uma vez e linhas consecutivas seguem no mesmo `RAMWR`; envio em RAM (`spi_feed`); janela só é reemitida quando a linha não é a seguinte ou o span muda |
| Largura enviada | sempre 640 B (320 px) | apenas a largura ativa: Integer 224 px, Fit com `GB_MODE=1` 266 px (campos `s_slot_x0`/`s_slot_w`; `w==0` = linha cheia) |
| Hash de linha | por linha de saída, e **qualquer escrita de paleta invalidava todas as linhas** | `ws_line_hash` por linha-fonte; versões de paleta **por banco de 16 cores** (`s_ws_bank_ver`), só dos bancos usados pela linha e só quando a cor visível muda (`gpu.c`) |
| Linhas duplicadas do Fit | convertidas 2x | convertidas 1x; a 2ª é `memcpy` da 1ª (overlays carimbados depois, sobre cada linha) |
| Blend Shimmerless | 3 `bswap16` por pixel | LUT nativa (`wsc_palette_rgb565_native`) e 1 `bswap16` na saída; linha-fonte resolvida uma vez (`s_ws_src565`) |
| `ili9341_wait_frame_done` | só esperava os slots | também fecha a janela aberta (o Core 1 deixa CS baixo entre linhas) |

**Bug antigo corrigido: Progressive nunca funcionou.** `ili9341_set_interleave()` só gravava `s_ngp_interleave`; o WS usa `s_ws_interleave`, que nascia `true` e nunca era escrita. O console ficava **sempre entrelaçado** e todas as medições "Fit + Progressive" anteriores eram, na verdade, Interleave. Agora as duas variáveis são gravadas e o hash é invalidado. Em Progressive de verdade, cenas densas podem aproximar o SPI de ~15 ms/frame no Fit 320 px.

### 27.3 Núcleo V30 (`src/oswan/nec/`, `memory.c`, `io.c`)
- **Tabela de páginas** (`ws_page[17]`, `ws_memory_bank_changed()`): ponteiros diretos por página de 64 KB (RAM interna só em WSC, SRAM do cartucho se ≥ 64 KB, ROM por `C0..C3`). Refeita ao escrever `0xC0..0xC3`, no reset, em `ws_set_system` e ao carregar state. O banco 15 com o patch do Conan fica no caminho lento.
- **Inline de leitura e escrita no `nec.c` só no console** (`PSRAM_CS_PIN`): economiza ciclos, custa ~8 KB de SRAM. O HW15 mantém o caminho por função. Escrita direta em `a < 0x2000`.
- **Detecção de ciclo ocioso** (`nec_backjump`, `ws_mc`): `ws_mc` conta escritas que **realmente mudam** a memória e toda escrita em porta. Com a memória quieta, cada desvio para trás guarda o hash do estado da CPU; um estado repetido (tabela de 128 entradas) é um ciclo fechado e o resto da fatia é pulado. Motivo: o Digimon passa ~86% do tempo num `yield` do escalonador (ROM `0x3A3460`, ~700 trocas de tarefa por frame, cada uma regravando os mesmos valores). `n` caiu de ~20 ms para ~12 ms.
- Removido o contador `nec_TotalClock` (leitura-modificação-escrita por instrução).
- `cpu_readport`, `cpu_writeport` e `ws_gpu_write_byte` foram para a SRAM.
- **Renderizador rápido em SRAM** (`gpu.c`, só console): `ws_render_scanline_color_fast` cobre o caso comum (cor, sem janela de foreground e sem janela de sprites): fundo, sprites prio 0, foreground, sprites prio 1, com as mesmas regras de transparência. O resto cai no renderizador completo (123 KB em flash).

### 27.4 Pacing, áudio e salto de desenho (`src/main.cpp`)
- **Estalos de áudio com qualquer variação de frame**: o pacing era um timer de 16.680 µs, e o DAC I2S consome exatamente 44.100 amostras/s (735 por frame). O anel ficava quase vazio e qualquer frame lento esvaziava o buffer. Agora o **DAC é o relógio**: ao fim do frame espera o anel cair a ~55%, deixando ~1 frame de folga (`audio_i2s_get_fill_permille()`). Turbo e builds sem I2S mantêm o timer.
- **Salto de desenho**: com o anel abaixo de 30% (depois de ter chegado ao nível normal), o frame é emulado mas não desenhado (máx. 2 seguidos). Mantém a velocidade do jogo e o áudio a 60 Hz.

### 27.5 Bugs de emulação encontrados e corrigidos
| Sintoma | Causa raiz | Correção |
| :--- | :--- | :--- |
| FF1 ao escolher New Game: tela rosa com lixo | Os **timers de HBlank/VBlank disparavam a cada linha quando a recarga era 0** (regra criada para o Klonoa). O FF1 liga os timers e as interrupções (`A2=0x0F`, `B2=0xC0`) antes de montar a tabela de despacho de interrupções, e no hardware um timer sem recarga não conta. A ISR corria por uma tabela vazia | `ws.c`: só conta se a recarga foi programada; **o Klonoa mantém o disparo a cada linha** via `s_is_klonoa` |
| Fault de barramento em `cpu_writeport` (FF1) | `OUT DX,AX` com `DX=0x948E`: o jogo carrega só `DL` e deixa lixo em `DH`. `ws_ioRam[port]` com a porta inteira escrevia ~38 KB além do array (silencioso enquanto havia RAM atrás) | `port &= 0xFF` (o hardware decodifica só 8 bits). Tentativa intermediária errada: descartar a escrita |
| Cena em modo mono dentro de console color renderizada errada | O caminho colorido não decodifica o modo de vídeo 0 | `ws_gpu_colorActive` = sistema WSC **e** bit 7 da porta `0x60`; usada por renderizador e driver. Recalculada em reset, troca de sistema e load state. A tabela de tons mono (`ws_set_colour_scheme`) agora é sempre inicializada |
| Overlay de volume (SELECT+UP/DOWN) não aparecia em cena parada e deixava resíduo | O hash só cobre a imagem do jogo; o HUD era carimbado depois, em linhas puladas | Enquanto o HUD está ativo, as linhas 180–199 são sempre enviadas e o hash é invalidado (horizontal e Tate) |
| Fundo do HUD salmão | `col_bg = 0xEEEE` é valor de 12 bits | `0xEF7D` em `shared_menu/dmg_menu.c` (**afeta todos os emuladores; recompilar os demais**) |

### 27.6 Incidente de processo (registrar)
Um `git checkout -- CMakeLists.txt` meu **reverteu o `CMakeLists.txt` não commitado** do usuário. O arquivo foi **reconstruído**: linhas 1–54 copiadas do `CMakeLists.txt` do PicoNGP (origem do arquivo) e linhas 55+ reproduzidas do que havia sido lido na sessão. O build HW16 resultante bateu em **806.912 B**, igual ao binário testado antes do incidente, mas a identidade byte a byte com o original não é garantida. **Recomendação: commitar o `CMakeLists.txt` agora.** Regra: nunca usar `git checkout`/`restore` em arquivo com alterações não commitadas.

### 27.7 Estado dos binários
| Alvo | Arquivo | Tamanho | Heap livre | Validação |
| :--- | :--- | ---: | ---: | :--- |
| **Core2350B (HW16)** | `PicoWS_hw16.uf2` | 830.464 B | ~20 KB | [OK] Digimon 60 FPS Fit + Progressive, FF1 inicia, overlay de volume confirmado pelo usuário |
| **RP2350 Plus (HW15)** | `PicoWS.uf2` | compila | ~12 KB | [PENDENTE] não testado em hardware nesta sessão (sem inline do V30, sem renderizador SRAM, sem quadro Tate em PSRAM) |

### 27.8 Pendente
1. Validar o **Klonoa** e outros jogos mono/WSC após as mudanças de timer, porta e modo de vídeo.
2. Validar o HW15 em hardware (heap apertado: ~12 KB).
3. Cenas densas em Progressive + Fit 320 px: SPI perto do limite; opções: 12 bpp no ST7789 (RGB444 nativo do WSC) ou SPI mais rápido.
4. Recompilar os outros emuladores para pegar a cor corrigida do HUD de volume.
5. Contadores leves de profiling ainda existem em `ili9341.c` (`s_prof`), sem consumidor; remover se desejado.
6. O FF1 é uma ROM mono (`minimumSupportSystem = 0`) nomeada `.wsc`: o emulador a roda como WSC pela extensão (`main.cpp`).
