# 🤖 Agents.md — PicoSMS & PicoGG (RP2350 + ILI9341)

Documentação técnica, plano de implementação e acompanhamento de progresso das operações de agente no projeto **PicoSMS** e **PicoGG**.

Firmwares alvo de saída:
- **`PicoSMS.uf2`** — Emulador Sega Master System para RP2350 + ILI9341
- **`PicoGG.uf2`** — Emulador Sega Game Gear para RP2350 + ILI9341

---

## 📋 Contexto e Diretrizes do Projeto

- **Local do Projeto:** `c:\dev\PicoDMG-Temp\PicoSMS`
- **Referência Principal:** `C:\dev\pico-infonesPlus` (PicoNES funcional com ILI9341, menus, in-game menu, áudio I2S, save/load states)
- **Referência Efeito Grid:** `C:\dev\PicoDMG-Temp\PicoGBC` (implementação de efeito LCD grid)
- **Referência Lógica SMS/GG:** `C:\Users\Michael\OneDrive\Documentos\Raspberry Pi Pico\PicoDMG-singlepartition` (`AgentSMS.md`, `AgentGG.md`, paleta GG 4+4+4, crop Sonic, interlace)
- **Microcontrolador Alvo:** **Raspberry Pi RP2350** (Waveshare RP2350 Plus 16MB / Pico 2)
- **Display:** **ILI9341 SPI TFT (320x240)** em SPI0 (Frequência ~31.25 MHz)
- **Áudio:** **DAC I2S MAX98357A** (DIN: GP26, BCLK: GP27, LRCLK: GP28)
- **Cartão SD:** SPI1 (MISO: GP12, CS: GP13, SCK: GP14, MOSI: GP15)
- **Controles:** GPIO 2 a 9 (UP=2, DOWN=3, LEFT=4, RIGHT=5, A=6, B=7, SELECT=8, START=9)
- **Compatibilidade:** `pico-bootLoader` (offset `0x10080000`)

---

## 🖥️ Mapeamento de Hardware (RP2350 — HW_CONFIG 15)

### 1. Display ILI9341 (SPI0)
| Sinal | GPIO | Função |
|---|---|---|
| CS | 17 | Chip Select SPI0 (ativo LOW) |
| SCK | 18 | SPI Clock |
| MOSI | 19 | SPI MOSI (TX) |
| DC | 20 | Data / Command |
| RST | 21 | Reset de Hardware |
| BL | 22 | Backlight (Controle de Brilho PWM / SIO) |

### 2. Áudio I2S — MAX98357A (PIO)
| Sinal | GPIO | Função |
|---|---|---|
| DIN | 26 | I2S Data Out |
| BCLK | 27 | I2S Bit Clock (Clock Base) |
| LRCLK | 28 | I2S Word Select / LRCLK (Clock Base + 1) |

### 3. Cartão MicroSD (SPI1)
| Sinal | GPIO | Função |
|---|---|---|
| MISO | 12 | SPI1 RX |
| CS | 13 | Chip Select SD (ativo LOW) |
| SCK | 14 | SPI1 Clock |
| MOSI | 15 | SPI1 TX |

### 4. Botões e Controles Físicos
| Botão | GPIO | Master System | Game Gear | Menu Clean Light / In-Game |
|---|---|---|---|---|
| UP | 2 | D-Pad Cima | D-Pad Cima | Mover Cursor para Cima |
| DOWN | 3 | D-Pad Baixo | D-Pad Baixo | Mover Cursor para Baixo |
| LEFT | 4 | D-Pad Esquerda | D-Pad Esquerda | Página Anterior / Ajustar Valor (-) |
| RIGHT | 5 | D-Pad Direita | D-Pad Direita | Próxima Página / Ajustar Valor (+) |
| A | 6 | Botão 2 (A) | Botão 2 (A) | Confirmar / Iniciar / Salvar |
| B | 7 | Botão 1 (B) | Botão 1 (B) | Voltar / Resumir Jogo |
| SELECT | 8 | Pause Console | — | — |
| START | 9 | Pause Console | Start GG | — |

**Atalhos e Combos:**
- **START + B**: Abre o Menu In-Game (Pausa).
- **START + A**: Liga / Desliga overlay de FPS.
- **SELECT + START (durante jogo)**: Salva SRAM e retorna ao seletor de ROMs.
- **SELECT + START (no seletor de ROMs)**: Reinicia para o `pico-bootLoader`.

---

## 🎯 Requisitos dos Binários de Saída

### 1. `PicoSMS.uf2`
- **Pasta de ROMs:** `0:/roms/ms/` (fallback `/roms/ms/`) — Extensões `.sms`, `.bin`
- **Pasta de Dados:** `0:/saves/ms/` (fallback `/saves/ms/`)
  - Configuração: `0:/saves/ms/picosms.cfg`
  - Saves de bateria: `0:/saves/ms/<jogo>.sav`
  - Save States: `0:/saves/ms/<jogo>_1.ss` até `_4.ss`
- **Efeitos de Imagem no Menu:** **Normal** | **Scanlines** (atenuação de 50% nas linhas ímpares)
- **Modos de Escala:** Fit (4:3 centralizado 256x192), Stretch (320x240 tela cheia), Integer (1:1 nativo centralizado)

### 2. `PicoGG.uf2`
- **Pasta de ROMs:** `0:/roms/gg/` (fallback `/roms/gg/`) — Extensões `.gg`, `.bin`
- **Pasta de Dados:** `0:/saves/gg/` (fallback `/saves/gg/`)
  - Configuração: `0:/saves/gg/picogg.cfg`
  - Saves de bateria: `0:/saves/gg/<jogo>.sav`
  - Save States: `0:/saves/gg/<jogo>_1.ss` até `_4.ss`
- **Efeitos de Imagem no Menu:** **Normal** | **Grid** (matriz LCD screen-door escurecendo 14% linhas/colunas)
- **Modos de Escala:** Fit (10:9 GG preenchendo 240 linhas -> 266x240 centralizado), Stretch (320x240 tela cheia), Integer (1:1 nativo 160x144 centralizado)

---

## 📅 Log de Atividades e Progresso

| Data/Hora | Ação | Status |
|---|---|---|
| 2026-09-14 18:05 | Análise do pedido do usuário e exploração do workspace PicoSMS | ✅ Concluído |
| 2026-09-14 18:07 | Análise dos projetos de referência: pico-infonesPlus, PicoGBC e PicoDMG-singlepartition | ✅ Concluído |
| 2026-09-14 18:10 | Criação do plano de implementação detalhado (`implementation_plan.md` e `agents.md`) | ✅ Concluído |
| 2026-09-14 18:15 | Sincronização dos submódulos `pico_shared`, `pico_lib`, `tusb_xinput` | ✅ Concluído |
| 2026-09-14 18:18 | Adaptação e extensão do driver `ili9341` para streaming SMS/GG com Scanlines e Grid | ✅ Concluído |
| 2026-09-14 18:20 | Implementação do seletor Clean Light e Menu In-Game no `main.cpp` com suporte condicional SMS/GG | ✅ Concluído |
| 2026-09-14 18:21 | Configuração de `CMakeLists.txt` para gerar os 2 alvos (`PicoSMS` e `PicoGG`) com relocação bootloader | ✅ Concluído |
| 2026-09-14 18:21 | Criação do script de compilação automatizado `build_for_bootloader.ps1` | ✅ Concluído |
| 2026-09-14 18:22 | Compilação e validação dos binários `PicoSMS.uf2` e `PicoGG.uf2` | ✅ Concluído |
| 2026-09-14 18:35 | Ajuste do modo FIT no PicoSMS para escalar 192 linhas para 240px de altura | ✅ Concluído |
| 2026-09-14 18:36 | Implementação de limpeza com quadro preto (`ili9341_clear_black`) na transição de menus | ✅ Concluído |
| 2026-09-14 18:36 | Recompilação e validação final dos binários atualizados `PicoSMS.uf2` e `PicoGG.uf2` | ✅ Concluído |
| 2026-09-14 18:47 | Correção do overlay de FPS (START + A) direto no pipeline do blitter e no menu Image | ✅ Concluído |
| 2026-09-14 19:05 | Otimizações críticas de performance para 60 FPS: overclock a 302.4 MHz, clk_peri anexado a clk_sys, SPI a 50.4 MHz (16-bit com CS mantido baixo), batch único de áudio I2S e pacing com busy_wait_us_32 | ✅ Concluído |

---

## Standby por Savestate e Resume (PicoSMS e PicoGG) (2026-11-05)

Camada comum: `AGENTS.md` da raiz, §45 (`shared_menu/dmg_power`, `dmg_resume`,
`dmg_boot_intro`). Aqui fica so o que mudou neste emulador.

Fluxo: chave OFF -> backlight apagado, painel pintado de preto, savestate no slot
`_R` (`<saves_dir>/<jogo>_R.ss`, nunca aparece no Load State 1..4), saves de
bateria gravados, `0:/saves/resume.bin`, DORMANT. Chave ON -> reboot, o bootloader
pula direto para o emulador, a abertura do bootloader (logo + som) roda no Core 1
enquanto o Core 0 carrega a ROM, e o `_R` e carregado depois do 1o quadro.
Desligar no seletor de jogos volta ao seletor, tambem com a abertura.

### O que foi feito
- `ili9341_write_rect_565()` no driver; `audio_i2s_flush()` no driver de audio.
- Descritor: `audio_free/push` (anel EXT 44,1 kHz), `flush_saves = system_save_sram()`,
  `audio_restore = sms_audio_restore` (flush + 44,1 kHz).
- Boot com `dmg_resume_check()`; `request_power_off` no limite de quadro.
- Gravacao da ROM em flash: compara o arquivo inteiro antes de apagar, entao um
  resume nao regrava a ROM.

### Correcoes dos testes
| Sintoma | Causa | Correcao |
| :--- | :--- | :--- |
| Ultimo quadro do jogo aparecia antes da abertura, logo "escorrendo" por cima | GRAM do painel sobrevive ao DORMANT e ao reset | `shared_menu` pinta o painel de preto antes de dormir e no inicio da abertura |
| Jogo lento depois do resume (normal apos Reset; Load State pelo menu normal) | `_R` carregado antes do loop, logo apos `system_load_sram()`, fora do caminho do menu de pausa | `s_resume_pending`: load dentro do loop, depois do 1o quadro, pela mesma sequencia do Load State do menu de pausa (parar blitter -> load -> religar -> resetar pacing) |

### Status
Validado em hardware (Core2350B) pelo usuario em 2026-11-05: OFF no jogo, no menu
de pausa e no seletor; volta no mesmo ponto, com som e FPS normais.
