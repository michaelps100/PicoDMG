# 🤖 Agents.md — PicoA26 (Atari 2600 para RP2350 + Display ILI9341)

Documentação técnica, arquitetura sem PSRAM, mapeamento de hardware, menus, save/load states e acompanhamento das tarefas do emulador **PicoA26**.

Firmware alvo de saída:
- **`PicoA26.uf2`** — Emulador Atari 2600 para Raspberry Pi RP2350 (sem PSRAM) com display ILI9341 SPI TFT.

---

## 📋 Contexto e Especificações do Projeto

- **Local do Projeto:** `/mnt/c/dev/PicoDMG-Temp/PicoA26`
- **Referência:** `PicoNES` (`/mnt/c/dev/PicoDMG-Temp/PicoNES/`) e `PicoSNES` (`/mnt/c/dev/PicoDMG-Temp/PicoSNES/`)
- **Microcontrolador Alvo:** **Raspberry Pi RP2350** (ex: Waveshare RP2350 Plus 16MB / Raspberry Pi Pico 2) **SEM PSRAM** (520 KB SRAM interna).
- **Display:** **ILI9341 SPI TFT (320x240)** em SPI0.
- **Áudio:** **DAC I2S MAX98357A** (DIN: GP26, BCLK: GP27, LRCLK: GP28) com sintetizador TIA de 2 canais nativo.
- **Cartão SD:** SPI1 (MISO: GP12, CS: GP13, SCK: GP14, MOSI: GP15) com FatFs.
- **Controles Físicos:** GPIO 2 a 9 com pull-up interno ativo (Active-LOW).
- **Compatibilidade com Bootloader:** `pico-bootLoader` (offset de partição `0x10080000`).

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
| CS | 13 | Chip Select SD |
| SCK | 14 | SPI1 Clock |
| MOSI | 15 | SPI1 TX |

### 4. Controles Físicos (GPIO Direto, Active-LOW)
| Botão | GPIO | Mapeamento Atari 2600 | Menu Clean Light / In-Game |
|---|---|---|---|
| UP | 2 | D-Pad Cima (SWCHA bit 4) | Cursor Cima |
| DOWN | 3 | D-Pad Baixo (SWCHA bit 5) | Cursor Baixo |
| LEFT | 4 | D-Pad Esquerda (SWCHA bit 6) | Página Anterior / Ajustar Valor (-) |
| RIGHT | 5 | D-Pad Direita (SWCHA bit 7) | Próxima Página / Ajustar Valor (+) |
| A | 6 | Fire / Botão de Ação (INPT4 bit 7) | Confirmar / Iniciar / Salvar |
| B | 7 | Fire Secundário | Voltar / Resumir Jogo |
| SELECT | 8 | Game Select Switch (SWCHB bit 1) | — |
| START | 9 | Game Reset Switch (SWCHB bit 0) | — |

**Atalhos e Combos:**
- **START + B**: Abre o Menu In-Game (Pausa).
- **START + A**: Liga / Desliga overlay de FPS.
- **SELECT + START (durante jogo)**: Retorna ao seletor de ROMs.
- **SELECT + START (no seletor de ROMs)**: Reinicia para o `pico-bootLoader`.

---

## 🎯 Diretórios e Destinos de Arquivos no SD Card

- **ROMs:** `roms/a26` (`0:/roms/a26/` com fallback `/roms/a26/` e `0:/roms/atari2600/`) — Extensões `.a26`, `.bin`, `.rom`
- **Saves, Configurações e States:** `saves/a26` (`0:/saves/a26/` com fallback `/saves/a26/`)
  - Configuração persistente: `0:/saves/a26/picoa26.cfg`
  - Estados salvos: `0:/saves/a26/<jogo>_1.ss` até `_4.ss`
- **Menu In-Game Image Settings:**
  - `Scale: < Fit > | < Stretch > | < Integer >`
  - `Effect: < None > | < Scanlines >`

---

## 📅 Log de Tarefas

| Tarefa | Descrição | Status |
|---|---|---|
| 1. Configuração do Ambiente e Submódulos | Cópia e estruturação de `pico_shared`, `pico_lib`, `tusb_xinput` | ✅ Concluído |
| 2. Criação do agents.md | Documentação completa de especificações e progresso | ✅ Concluído |
| 3. Atualização do TIA e Vídeo | Suporte a cores RGB565 de alta fidelidade e frame buffer TIA | ✅ Concluído |
| 4. Sintetizador de Áudio TIA para I2S | Emulador de 2 canais com LFSR e saída I2S via DMA (MAX98357A) | ✅ Concluído |
| 5. Gerenciador de Cartuchos & Bankswitching | Execução direta em SRAM até 64KB (2K, 4K, 8K F8, 16K F6, 32K F4, 64K EF) | ✅ Concluído |
| 6. Save & Load States | Serialização de CPU, RIOT, TIA e cartuchos em slots 1-4 (`.ss`) | ✅ Concluído |
| 7. Menu Clean Light & Menu In-Game | UI v5 Clean Light, menu de pausa (START+B), combos e ajustes | ✅ Concluído |
| 8. Arquitetura Multicore (Core 1 Blitter) | Pipeline assíncrono para ILI9341 com suporte a Scanlines | ✅ Concluído |
| 9. CMake & Compilação Final | Script Linux executado com sucesso gerando `PicoA26.uf2` | ✅ Concluído |
| 10. Adaptação Render xrip & ILI9341 Burst | Sincronismo estrito de VSYNC/VBLANK (37 lines) + burst SPI contínuo de 192 linhas | ✅ Concluído |
| 11. Correção de CPU 6507, TIA e RIOT | Modo decimal em ADC/SBC, borrow correto em SBC, sticky collision latches, RIOT contínuo | ✅ Concluído |
| 12. Multiplexação 48-Pixel & Colisão | Latching GRP0/GRP1 (VDEL), amostragem direta on-the-fly, eliminação de falsas colisões no VBLANK | ✅ Concluído |
| 13. Alinhamento de Escada (Ball/HMOVE) & VDEL Cruzado | Transferência cruzada VDELP0/1 e VDELBL, alinhamento exato de strobe RESBL/HMOVE, STA (zp),Y | ✅ Concluído |
| 14. Substituição do Core por DenizBasgoren/atari2600emu | Transição para core ciclo-a-ciclo completo com tabela Stella HMOVE, BCD nativo, pipeline TIA e bridge a26_core | ✅ Concluído |
| 15. Otimização Crítica de Performance (60 FPS) | Matemática O(1) de sprites, cache dirty de sprites, fusão pixel+colisão, bypass VBLANK, execução em SRAM | ✅ Concluído |
| 16. Correção dos Controles do Enduro e Otimização Extrema | RIOT SWCHA/SWACNT (ORA+DDR), latching INPT4 (VBLANK bit 6), inlining de flags da CPU e ocupação de vídeo, eliminação de 300k chamadas/frame | ✅ Concluído |
| 17. HW_CONFIG 16 (Core2350B) + painel ST7789 | Ver secção 8 | ⚠️ Compila, não validado em hardware |
| 18. Correções de emulação (áudio, travamentos, pacing) | Ver secções 7 a 9 | ⚠️ Compila, parcialmente validado |
| 19. Harness Windows + desempenho (1.4-perf-r4) | Ver secção 16 | ✅ Framerate validado no Core2350B |
| 20. Controles do Enduro (1.5-enduro-r5) | Ver secção 17 | ✅ Validado no Core2350B |

---

## 🧠 Análise de Lições Aprendidas e Restrições Técnicas (Histórico de Rollbacks)

1. **Preservação Absoluta do Encadeamento de Sprites (OAM):** o NeoPop implementa
   encadeamento de sprites (`lastSpriteX`/`lastSpriteY`). Sprites invisíveis ou fora
   de ecrã **ainda atualizam os acumuladores** dos seguintes. Qualquer otimização
   deve procesar rigorosamente os 64 elementos na ordem original.

2. **Sweet Spot dos Clocks:** CPU a **360 MHz** com VREG a 1.30 V; `clk_peri`
   desacoplado via `PLL_USB` a 140 MHz, divisor de hardware 2, SPI a 70.0 MHz.

3. **Restrição Severa de SRAM Interna:** nenhuma otimização nova pode alocar
   buffers intermediários estáticos grandes na SRAM; o espaço livre deve
   permanecer acima de 40-50 KB para stack, heap e buffers do FatFs.

4. **Alocação e liberação casam (regra permanente):** um bloco da PSRAM
   (lwmem / `Frens::f_malloc`) passado a `free()` — ou vice-versa — **corrompe o
   heap** e trava o MCU. Toda alocação condicional precisa de um flag de qual
   allocator foi usado, consultado na liberação. Aconteceu duas vezes no
   PicoMD.

5. **Nunca pôr um framebuffer na stack:** a stack do core0 do PicoA26 é
   `PICO_STACK_SIZE = 4096` bytes. Um array local de 2.560 bytes no overlay
   rebentou-a e crashou o jogo (ver 10c).

6. **Exclusão mútua no SPI partilhado entre cores:** dois cores a conduzir o
   mesmo periférico sem lock dá `while (spi_is_busy())` eterno no SDK. Existe
   agora um handshake (ver 10c).

7. **Verificar o `$LASTEXITCODE`, não só `Test-Path`:** um `ninja` que falha deixa
   o `.uf2` anterior no lugar, e o script reportava `SUCESSO` com código velho.
   Corrigido em todos os scripts de build.

---

## 📊 Estatísticas da Compilação

### `PicoA26.uf2` (HW_CONFIG 15, RP2350 Plus)
- **Tamanho:** 477.184 bytes
- **Offset de Partição (Bootloader):** `0x10080000`
- **SRAM (heap medido no `.map`):** 113.552 B livres
- **Carregamento de ROMs:** Zero-copy direto do SD para o buffer `s_cart_rom` de
  64 KB na SRAM, com stripping de cabeçalhos (128/32/64 bytes)
- **Controle de LED:** apagado por omissão; acende só durante a leitura da ROM
- **Script de Build:** `.\build_for_bootloader.ps1` (Pico SDK 2.3.1)

### `PicoA26_hw16.uf2` (HW_CONFIG 16, Core2350B)
- **Tamanho:** 487.936 bytes
- **SRAM (heap medido no `.map`):** 112.668 B livres
- **Painel:** ST7789 (runtime, via `ST7789=` em `/boot.txt`)
- **Script de Build:** `.\build_core2350b.ps1`

---

## 7. Áudio era apenas ruído — `atari/atari_audio.c` + `main.cpp`

Duas falhas independentes, ambas suficientes para produzir "só um ruído, em todos
os jogos".

### Taxa de amostragem 2,8× demasiado lenta (a causa do ruído)

`a26_audio_step_cb` era chamada **uma vez por scanline** e enfileirava **uma**
amostra. O DMA do I2S consome a `DVIAUDIOFREQ` = 44.100 Hz, mas 262 linhas ×
60 Hz = **15.720 amostras/s**. O anel era esvaziado 2,8× mais rápido do que era
enchido, e o DMA lia repetidamente o que restava dele. *Underrun* garantido em
todos os jogos, sem excepção — daí não ser um problema de um jogo mal emulado.

Corrigido com um acumulador em ponto fixo de 2,805 amostras por linha
(44100/15720), emitindo 2 ou 3 por linha alternadamente de forma que a média é
exacta ao longo do tempo.

**Isto também corrigiu o tom.** Os divisores de canal do TIA são clockados uma
vez por amostra emitida, e 1/AUDF está em unidades dessas chamadas — a 15,7 kHz
todas as notas estavam ~2,8× baixas. A 44,1 kHz, AUDF=0 dá um período de ~22 kHz.

### Offset DC dependente do jogo

A mixagem era `(sample0 + sample1 - 15000)`, e 15000 é o máximo de **um** canal.
Um jogo **silencioso** produzia `-15000` constantes em cada amostra, e a meia
volume `-7500`. É um offset DC grande e variável por jogo directo no
amplificador I2S, que satura o MAX98357A — a mesma classe de bug que silenciou
o NGP.

Corrigido para saída **bipolar por canal** (`±AUDV`): um canal silencioso dá
exactamente 0 e um canal activo fica acoplado a AC em torno de 0, logo o offset
é zero por construção.

> **Validado pelo utilizador:** a correcção da taxa + offset DC **melhorou o
> áudio de forma perceptível**, confirmando o diagnóstico do underrun.

### 7b. Modos de waveform — histórico corrigido em 19/10/2026

As descrições anteriores de AUDC=0 como ruído branco de 15 bits a ~595 kHz,
AUDC=5 como poly4 e AUDC=B como poly4 estavam incorretas. O ruído branco é
AUDC=8 (poly9); 0 e B produzem nível constante; 4 e 5 são tons /2. A tentativa
anterior foi revertida, mas isso não validava sua explicação teórica.

Agora o sintetizador avança a 3579545/114 Hz e é reamostrado separadamente para
44100 Hz. Os períodos de todos os modos são testados no host. O filtro DC é
necessário: a conversão bipolar não elimina offset de onda constante ou com
duty cycle desigual. A qualidade audível desta revisão aguarda escuta no console.

### 7c. START + A: o overlay era desenhado na zona morta

Primeira implementação desenhava o texto em **x = 8**, que na janela do Game Boy
Classic cai na **borda preta (x = 0..44)** — exactamente onde a carcaça tapa o
painel. Corrigido com o mesmo deslocamento que os outros emuladores usam:
`x = dmg_is_gb_mode() ? 53 : 0`.

O handler também não existia de facto: alternava o flag e gravava a config sem
**nunca chamar o driver**, não tinha detecção de borda (segurar alternava a cada
frame), tinha um `sleep_ms(300)` que congelava o jogo 0,3 s, e não suprimia
START/A para o jogo. Tudo corrigido seguindo o padrão do PicoNGP.

O overlay fica **acima** da janela do blitter (linhas 24..215), numa banda que o
jogo nunca toca, por isso não precisa de ser carimbado a cada frame. O contador
mostrado é o **FPS realmente medido**, actualizado uma vez por segundo — o que
dá um número honesto e serve de diagnóstico para o sintoma "jogos lentos".

---

## 8. HW_CONFIG 16 — Waveshare Core2350B + painel ST7789

Adicionado ao `pico_shared/BoardConfigs.cmake` com `PICO_BOARD` em `FORCE` (sem
isso o VS Code reescreve para a board RP2350A, **onde o GPIO 47 não existe**), LED
em GP39 e o bloco de I2S MAX98357A em GP26/27/28 (obrigatório: `USE_I2S_AUDIO`
vazio quebra todos os macros `EXT_AUDIO_*`).

O painel deixou de ser escolha de build: `dmg_panel.c` entrou no build,
`DEFAULT_PANEL_ST7789` (default `0`, `1` no HW_CONFIG 16), `ili9341_init()` foi
dividida em `st7789_init_sequence()` / `ili9341_init_sequence()`, e o `MADCTL`
hardcoded `0x28` passou a vir de `dmg_panel_get_madctl()` — **ST7789 = `0x60`**.

**A PSRAM do Core2350B não é usada pelo cartucho, deliberadamente.** O maior ROM
comum do 2600 é 64 KB e já corre zero-copy no buffer `s_cart_rom` de 64 KB na
SRAM; não há ganho. Este alvo existe pelo painel.

### Bug corrigido no script de build

`build_for_bootloader.ps1` só verificava `Test-Path` no `.uf2` após o `ninja`.
**Um `ninja` que falha deixa o `.uf2` anterior no lugar**, portanto o script
copiava binário velho e imprimia `[SUCESSO]`. Validado na prática: durante esta
sessão o script imprimiu `[ERRO] ninja falhou (exit 1)` num erro de compilação
real, onde antes ter-se-ia visto sucesso. `build_core2350b.ps1` já nasceu com a
verificação.

---

## 9. Correções de robustez em `core2600/`

### 9a. Opcode desconhecido matava o emulador — `cpu.c`

O `default:` do switch da CPU fazia `exit(1)`. Este core não implementa 8 opcodes
(`6B 8B 9B 9C 9E 9F 93 AB` — RLA, XAA, SHA, TAS, SHY, SHX, AHX, LAX), como o
próprio cabeçalho do ficheiro admite. Vários títulos Activision/Imagic do fim dos
70 usam-nos.

`exit(1)` no console é um **travamento permanente**: não há caminho de volta ao
bootloader e nada no loop de runtime se recupera. Passou a ser um **NOP de 2
ciclos**, com log rate-limited (16 mensagens) para um jogo preso em loop no
opcode não poder inundar a UART. O NOP preserva o orçamento de ciclos, logo o
timing de kernel por WSYNC não desliza.

### 9b. `a26_core_reset()` era incompleto

Sobrevivia ao reset e contaminava o jogo seguinte:

| Estado | Consequência |
| :--- | :--- |
| `timer_secondary_value` | O loop testa `--timer_secondary_value <= 0` a cada ciclo de máquina. Timer deixado expirado dispara `timer_tick_underflow()` **em todo ciclo** desde o primeiro frame: **INTIM free-runs**. Qualquer jogo que use TIM1T/INTIM como contador de frames ou RNG fica partido, e o reset do menu não reparava. |
| `cpu.is_killed` | Faz latch num `HLT` e é testado no topo de `cpu_run_for_one_machine_cycle()`. **Nada na árvore o limpava** — um `HLT` num jogo matava a CPU pelo resto da sessão. |
| `s_phi2_cnt` | Era `static` dentro de `a26_core_run_frame()`, impossível de resetar. É a fase CPU/beam (`tv.x / 3`); carregá-la reposiciona a CPU contra o feixe e estraga a tabela HMOVE. Passou para escopo de ficheiro. |
| latches de input, CXCLR, `s_ora`/`swacnt` | `input_reset_latches()`, `video_CXCLR_write()` e `input_reset_ports()` agora correm no reset. |

`input_reset_ports()` repõe `s_ora`, `s_orb`, `swacnt`, `swbcnt` e `swcha`. Sem
isto, um `SWACNT = 0xFF` deixado pelo jogo anterior (típico de uma rotina de init)
fazia `SWCHA` devolver o valor velho do strobe nos bits 4..7, com o joystick morto.

### 9c. Deadlock core0/core1 no `ili9341_blitter_stop()`

A ordem estava invertida: limpava `s_blitter_active` **antes** de drenar
`s_frame_ready`, e o core1 estaciona em `if (!s_blitter_active)` **sem nunca
limpar essa flag**. Se o core0 limpasse a flag com um frame pendente, ambos os
cores morriam — sem watchdog para notar. Atingido a cada `START+B`. Corrigido
invertendo: drenar o frame **antes** de baixar a flag.

### 9d. Pacing sem acumulador — `main.cpp`

O código fazia `sleep(target - elapsed)` e depois **recarimbava
`frame_start_time` a partir do instante atual**. Cada iteração tinha o seu próprio
orçamento de 16666 µs medido desde o fim da anterior — não existe plano partilhado,
logo **nada é recuperado**: um frame atrasado atrasa todos os seguintes, e o jogo
entra em câmara lenta permanente.

A margem era quase nula: o blitter sozinho gasta ~15,7 ms dos 16,67 ms a empurrar
122.880 bytes a 62,5 MHz.

Substituído por um acumulador de **deadline fixo**: o alvo avança sempre um
período exacto, logo um frame atrasado *pega emprestado do seguinte*. Se ficar mais
de um frame inteiro atrás, ressincroniza em vez de sprintar para recuperar.

### 9e. Watchdog nunca estava activo

`watchdog_enable()` não era chamado em lado nenhum do runtime do PicoA26 (só no
helper de escrita de flash e no launcher legacy FruitJam, inalcançável), e
`watchdog_update()` não aparecia em `main.cpp`, `core2600/` nem no blitter. Todo
travamento era portanto permanente.

Activado com 6 s no loop de jogo, alimentado em `a26_core_run_frame()` na volta de
cada scanline e nos dois spins do blitter. Os 6 s são deliberadamente generosos.

### 9f. Identificação de cartuchos — corrigida em 19/10/2026

Associar toda ROM de 64 KB a E7 estava errado: o mapper local E7 endereçava
apenas 16 KB. Agora há tamanhos exatos e assinaturas para os mappers suportados,
incluindo EF/F0 em 64 KB, E0/FE/E7 em 8 KB e Superchip. 12 KB também não implica
exclusivamente FA: existem imagens E7 de 12 KB. DPC (Pitfall II), ARM/CDF/DPC+
e 3E/3F identificados são recusados explicitamente. O limite de 64 KB não
significa suporte a todo mapper desse tamanho.

### 9g. Fila de escritas atrasadas demasiado pequena — `delayed_writer.c`

`DWLEN` era **5** e `write_after_delay()` **descartava a escrita em silêncio**
quando a fila enche (`if (i == -1) return;`). O Enduro faz scroll fino do playfield
em HBLANK, que é precisamente uma rajada de escritas em
`PF0/PF1/PF2/GRP0/GRP1/HMOVE`. Com 5 slots isso transborda, e um `PF*` perdido é
uma scanline permanentemente errada.

Subido para **32**. Custo: ~324 bytes de `.bss` e um scan linear ligeiramente mais
longo, que na prática é curto porque o primeiro slot livre costuma estar à frente.

### 9h. Descodificação RIOT — histórico corrigido em 19/10/2026

A alteração anterior por A3 estava errada. Nas leituras, address & 7 seleciona
SWCHA/SWACNT/SWCHB/SWBCNT (0..3), INTIM (4/6) e INSTAT (5/7). Assim, $0288
espelha SWCHA, não INTIM ($0284). Nas escritas, A2 seleciona o grupo e A4
distingue controle de borda de carga do temporizador; os temporizadores
canónicos são $0294..$0297. A3 espelha ambos.

A implementação atual testa espelhos, divisores, underflow, leitura de INTIM
no ciclo de underflow e latch de borda PA7. O bit 6 de INSTAT é PA7, não um
segundo flag de underflow. Estes testes corrigem a justificativa antiga;
não constituem validação física do Enduro.

---

## 10. Overlay de FPS (START + A) — o que fica, e o que foi corrigido

O overlay de FPS é uma funcionalidade pedida, não instrumentação, e **foi
mantido**. Três defeitos seus foram corrigidos:

| Defeito | Correcção |
| :--- | :--- |
| Array de 2.560 / 1.600 bytes na **stack** (que é `PICO_STACK_SIZE = 4096`) → crash ao exibir | Buffer `static` de 15.360 B em `.bss`. Nunca pôr um framebuffer nesta thread. |
| Largura fixa de 312 px a partir de `x = 53` chegava a `x = 364` num painel de 320 → **tela branca** (o endereço embala) | Largura limitada ao painel: 11 glifos em modo GB, 13 em tela cheia. |
| Overlay escrevia em SPI0 **sem exclusão mútua** com o core 1 → `while (spi_is_busy())` do SDK nunca completava, **travamento** | Handshake `s_spi_ovl_request` / `s_spi_ovl_idle`, servido no topo do loop do core 1 (sempre entre bursts), com limite de iterações. |
| Esperas `while (s_frame_ready)` infinitas | `a26_ovl_wait_blitter()` com tecto de iterações. |

Escala **3×** (24 px por glifo), a maior que cabe nas 24 linhas livres. A imagem do
jogo ocupa as linhas 24..215, portanto **0..23 e 216..239 estão livres**.

Custo de SRAM: o buffer de 15.360 B tirou o heap de ~127 KB para ~112 KB.

### 10b. Diagnóstico do Enduro — INSTALADO E RETIRADO

Foi instalado um overlay de diagnóstico (SELECT + B) com os registos do RIOT
(SWCHA, SWACNT) e os bits dos botões, para distinguir "o input não chega ao
jogo" de "o jogo lê mas a lógica está errada".

**Retirado a pedido do utilizador.** O overlay custou mais do que rendeu, e cada
uma das falhas era minha: uma versão crashou o jogo (stack), a seguinte deu tela
branca (largura fora do painel), a seguinte travou o microcontrolador (SPI sem
exclusão mútua). **Duas tentativas de corrigir o Enduro já se provaram erradas**
(secções 9h e 9b), e a instrumentação que devia desambiguar passou ela própria a
ser o problema.

Ficaram no código, porque são correcções reais e necessárias ao overlay de FPS: o
handshake de exclusão mútua de SPI0 e o clamp da largura ao painel.

---

## 11. Histórico anterior à revisão de 19/10/2026 — Enduro por diagnosticar

Sem instrumentação e sem mais tentativas às cegas. Candidatos identificados por
leitura de código, **nenhum verificado em hardware**:

| Ficheiro | Suspeito |
| :--- | :--- |
| `a26_core.c` | `a26_core_run_frame()` não tem orçamento de ciclos — o frame termina na queda de VSYNC, com tecto de 320 linhas. `NTSC_TICKS_PER_FRAME` (59659) existe no header e nunca é referenciado. |
| `a26_core.c` | `visible_line` satura silenciosamente em 192, o que corta o fundo da imagem. |
| `a26_core.c` | WSYNC é libertado exactamente em `tv.x == 160`, sem tolerância. |
| `video.c` | `video_RSYNC_write` faz `tv.x = 157` **sem resetar `s_phi2_cnt`**, re-fasando a relação CPU/beam. |
| `memory.c` | Tipo de cartucho ainda por tamanho: 8K pode ser E0 em vez de F8, e **EF (Firebird 64K) não tem ramo nenhum**. |
| `atari_audio.c` | Ruído branco (AUDC=0x0) e `0xB` por implementar; `0x6`/`0xA` são aproximações grosseiras. |

**Nota sobre a taxa de áudio:** as 2,805 amostras/linha assumem 262 linhas/frame.
Um orçamento de ciclos exacto resolveria este e o item do frame ao mesmo tempo.

`BRK` (`cpu.c`) não tem `return` e cai em `case 0x18 // CLC`. Inofensivo hoje
porque `cpu.cycle` é posto a 1, mas é um bug latente.


## 12. Revisão de integração RP2350 — 19/10/2026

Esta secção e docs/EMULATION_FIXES.md descrevem a implementação atual. As
secções anteriores são histórico e não comprovam funcionamento em hardware.

- Mantidos core existente, HW_CONFIG 15/16, 280 MHz, menus, pinagem e SRAM.
- SPI efetivo é impresso; a 280 MHz, pedir 62,5 MHz resulta em ~46,7 MHz.
  Blitter ocupado descarta apresentações sem bloquear execução e áudio.
- Pacing segue os clocks executados; VSYNC termina frames. Um teto absoluto de
  91200 clocks limita inclusive ROMs que escrevem RSYNC continuamente.
- Áudio TIA a ~31,4 kHz, saída racional a 44,1 kHz, filtro DC. DMA mantém
  silêncio durante underrun; somente a ISR agenda o próximo bloco.
- Reset limpa CPU, RAM, mapper, TIA, filas, caches, fases, áudio e inputs.
- Estados v4 incluem identificação/CRC da ROM, CRC dos dados, fases, áudio,
  inputs e escritas pendentes. Apenas o mapper ativo é serializado. V3 é recusado.
- Snapshots e FIL são estáticos; save/load não alocam buffers grandes na stack.
- Corrigido stride de 2 bytes/pixel do overlay e limpeza ao desligar START+A.
  Não foi adicionado overlay de diagnóstico.
- Esperas entre cores têm timeout de 100 ms e não alimentam watchdog. Erros são
  consultáveis; core1/SPI são reiniciados antes de permitir escrita do menu.
- Os oito opcodes antes ausentes têm implementação; XAA/LXA usam constante EE
  como convenção NMOS. BRK não cai em CLC. JAM retorna ao seletor.

**Validação física pendente:** Enduro, Pitfall, áudio, tempos reais, menus,
combos e slots 1–4 nos dois consoles. Testes de host e builds não demonstram que
todos os jogos funcionam nem que o hardware atinge 60 FPS. Valores atuais de
UF2, margem de heap e stack estão em docs/build-validation.json.


## 13. Build para teste no console — MCUME audio/perf r1 (19/10/2026)

A pedido do utilizador, não executar testes no WSL. A ronda anterior foi
reprovada pelo utilizador no hardware; os resultados antigos de host não
validam estas alterações nem comprovam melhoria no RP2350.

Referência consultada e guardada: MCUME_pico2/picovcs, commit
27f6b906aca34e06d6647bdca8215e25f8d20aa5. Foi integrado apenas Tiasound.c
(Ron Fries); o core CPU/TIA existente não foi substituído pelo x2600 do MCUME.
A síntese de áudio escrita na ronda anterior foi substituída pelo PCM dessa
referência, mantendo o relógio nativo /114 e saída I2S separados. Estados v5
incluem o estado do sintetizador de referência; v3/v4 são recusados.

O TFT passa a pedir 70 MHz, como o driver do PicoNGP local: a clk_peri de
280 MHz isto dá 70 MHz efetivos, contra os 46,7 MHz do pedido anterior.
O limite teórico de envio de 320x192 RGB565 cai de 21,1 ms para 14,0 ms;
é tempo de fio, não FPS medido. CPU permanece em 280 MHz.

Retirados do caminho quente: incremento de 64 bits em cada color clock,
agendamento de áudio em 2/3 das iterações, chamada a timer_tick por ciclo,
poll de PA7 em cada ciclo e impressão UART de FPS a cada segundo. PA7 é
atualizado quando inputs, ORA ou DDR mudam. Não há novo overlay de diagnóstico.

Estes UF2 são candidatos para teste físico pelo utilizador. Compilar não
significa que áudio, velocidade, imagem e controles melhoraram no console.

## 14. Retorno físico e candidato perf-r2 — 19/10/2026

O utilizador reprovou o resultado anterior: Enduro com imagem incorreta e carro
preso à esquerda; Pitfall com áudio ruim e cerca de 24 FPS. A fotografia enviada
mostra uma faixa deslocada no céu. Estes relatos prevalecem sobre os testes
antigos; não há confirmação física de melhoria nesta ronda.

Mudanças concretas no caminho quente, sem alterar CPU de 280 MHz:
- Escritas atrasadas: máscara de ocupação de 32 bits, visitando somente entradas
  pendentes. Mantidos índices, ordem de aplicação e pós-decremento dos atrasos.
  Importação de states reconstrói a máscara; formato v5 permanece compatível.
- Leitura de ROM 2K/4K/F8/F6/F4 evita o decodificador geral. Hotspots continuam
  selecionando o banco antes da leitura; mappers especiais usam o caminho geral.
- Distâncias NUSIZ de 16/32/64 usam máscara e comparação, removendo divisão e
  resto por pixel. Colisões continuam latched, gravando somente pares presentes.
- Callbacks TIA e Tia_process_2 passam a executar na SRAM. Síntese de áudio e
  frequência permanecem as da integração MCUME anterior.

Controles: conferidos GPIO 2..9, combos e bits 7/6 de SWCHA. Não foi encontrada
causa demonstrada para o carro preso, portanto nenhum remapeamento foi aplicado.
Foi consultada a listagem pública Enduro (Genesis Controller) - Omegamatrix.asm
em milnak/atari-vcs-disassembly apenas como referência de leitura; ela é uma
versão modificada, não comprova equivalência à ROM usada pelo utilizador.
O nome/variante da ROM foi solicitado. Gráficos e controles continuam pendentes.

Compilações ARM nativas Windows concluídas com código de saída 0 nos dois alvos:
- HW15: UF2 478208 bytes; RAM 433808/524288 bytes, margem 90480 bytes.
- HW16: UF2 488960 bytes; RAM 434696/524288 bytes, margem 89592 bytes.
- PSRAM: 0 bytes. Identificação SDK: 1.2-perf-r2.
- Logs: build_bootloader/perf-r2-build.log e build_hw16/perf-r2-build.log.

Não foram executados testes de host, WSL ou hardware nesta ronda. Compilação
comprova somente geração do firmware; não comprova FPS, áudio ou correção do Enduro.

## 15. Candidato 1.3-region-r3 — 19/10/2026

Ver docs/build-region-r3.md para alterações, referências, limites e evidências.
Corrigidos suporte regional removido da adaptação, altura de captura e ordem de
prioridade no modo score. Colisões agora usam tabela de 128 bytes e latches de
16 bits; CPU tem atalhos de ROM 2K/4K e RAM, preservando efeitos do mapper FE.
States v6 incluem região e registradores de cor; versões antigas são recusadas.

Builds Windows ARM finais com exit 0: HW15 486912 B (RAM 433468 B), HW16 497664 B
(RAM 434356 B). Sem PSRAM, sem aumento do clock de 280 MHz. Nenhum teste de host,
WSL ou hardware executado. O defeito de controle do Enduro continua sem causa
demonstrada; não foi registrado como corrigido. FPS e áudio precisam de teste
físico. Detecção por linhas não distingue SECAM/PAL nem PAL60/NTSC sem metadados.

## 16. Candidato 1.4-perf-r4 — harness no PC e desempenho medido (19/10/2026)

Primeira ronda com verificação real, não só compilação.

**Harness Windows** (`build_host.ps1` → `build_host_win/a26run.exe`, MSVC, sem
WSL): roda o core2600 com ROM real e pad roteirizado, gera PNG por frame, WAV
44,1 kHz (mesmo `audio_cb` do firmware) e CSV com hash por frame.
`tools/host_runner/golden.sh <pasta_roms> [record]` compara hash de vídeo+áudio
de Pitfall, Enduro, River Raid, Freeway, Space Invaders e Combat (1500 frames,
com input) contra `tools/host_runner/golden/hashes.txt`. ROMs vêm do SD, não do repo.

**Diagnóstico.** No harness, o código atual já joga Pitfall (placar 2000, 20:00,
logo Activision, Harry anda) e Enduro (START inicia, motor ~245 Hz sem DC).
**Correção (ver §17):** a afirmação de que esquerda/direita funcionavam no Enduro
estava errada — a cena rolava pelas curvas, não pela direção. Resultado idêntico com `char` unsigned (/J).
O UF2 testado no console era de 19:14, anterior a mudanças em `a26_core.c`/`video.c`;
as fotos do Pitfall são de 18/09. O problema medido é desempenho: o ELF ARM,
executado no Unicorn (`armprof.py`, scratchpad), gastava 5,34 M instruções/frame
no Pitfall, acima dos 4,67 M ciclos/frame de 280 MHz. Enduro mudo sem START é o
comportamento real do jogo.

**Mudanças (todas com golden idêntico bit a bit):**
- `A26_RAMFUNC` (`platform.h`): `cpu_bus_read/write`, BCD, flags, timer, input,
  AUDx e `Update_tia_sound` estavam na flash, chamados por veneer a cada acesso da
  CPU emulada. Agora na SRAM. `watchdog_update` a cada 32 linhas.
- Render preguiçoso: `a26_video_sync()` renderiza o trecho passado antes de toda
  escrita TIA, escrita atrasada, leitura de colisão, wrap e fim de frame;
  `video_render_span()` com tabela de classe por máscara; RSYNC/reset/load fazem rebase.
- Laço avança por ciclo de CPU quando não há escrita atrasada; WSYNC salta direto a x=160.
- Timer e áudio adiados (`a26_catch_up()`), reproduzidos por evento antes de acesso
  ao RIOT, escrita AUDx e no fim do frame.
- Coverage limpa só as palavras marcadas pelo objeto.

Instruções/frame (ELF HW16): Pitfall 2,51 M, Enduro 2,82 M, River Raid 2,72 M,
Space Invaders 2,70 M. CPU mantida em 280 MHz: 360 MHz forçaria SPI 60 MHz (USB
host nativo prende o PLL_USB) e a janela 320x192 levaria ~16,4 ms, perdendo frames.
PSRAM não usada: o gargalo é CPU, não SRAM (≈73 KB livres; PSRAM é mais lenta).

UF2: `PicoA26.uf2` (HW15) e `PicoA26_hw16.uf2` (HW16), exit 0.
**Validado em hardware (Core2350B / HW16):** o usuário confirmou que o framerate
ficou resolvido.

## 17. Candidato 1.5-enduro-r5 — controles do Enduro (19/10/2026)

**Sintoma (console, r4):** FPS ok; no Enduro o joystick não fazia nada e o carro
derivava. Reproduzido no harness: com esquerda, direita ou nada, a RAM de
"esquerda" era idêntica à de "nada" e o carro ficava parado na tela.

**Diagnóstico (harness + desmontagem da ROM):**
- O input chegava certo (SWCHA=$BF com esquerda, INPT4=$00 com fire).
- A rotina de direção (`$F529 BIT SWCHA`) só roda com `$A3 == 0`. `$A3` é o
  estado de derrapagem, ligado em `$F2AF/$F2B9` por colisão M0–P0 / P0–BL.
- `$A3` ficava ligado desde o frame 9, sem input. A colisão P0–BL vinha do
  painel (y≈214): `RESP0` acontece com P0 em largura dupla, depois o kernel
  troca NUSIZ0 para largura normal. O core somava o +1 dos jogadores largos
  na posição no instante do strobe, e esse +1 ficava; P0 ia a 57..64 e tocava a
  bola em 64 — colisão fantasma em todo frame, derrapagem eterna, joystick
  ignorado. O core MCUME (`tools/host_runner/vcsref.c`, só referência) não
  derrapava sem input.

**Correção (`core2600/video.c`):** RESPx guarda a posição do strobe; o +1 de
largura dupla/quádrupla é aplicado no desenho (`video_player*_effective_x_inline`,
usado pelo coverage e pelo míssil preso ao jogador), seguindo o NUSIZ vigente.

**Verificação no harness:** sem input, `$A3=00`, carro centrado sob o ponto de
fuga, ultrapassa carros (contador 200→196), odômetro avança; esquerda/direita
movem o carro em sentidos opostos (`$AE` +$11 / −$15). Golden: Pitfall, Freeway,
Space Invaders e Combat idênticos; Enduro e River Raid mudam (sprites largos com
troca de NUSIZ) e foram re-gravados após conferência visual. Desempenho
inalterado (Pitfall 2,51 M, Enduro 2,81 M instr/frame no ELF HW16).

UF2: `PicoA26_hw16.uf2` 503.296 B, `PicoA26.uf2` 492.544 B, exit 0.
**Validado em hardware (Core2350B / HW16, 19/10/2026):** o usuário confirmou que
os controles do Enduro funcionam no console.

### Como verificar mudanças futuras no core

1. `.uild_host.ps1` gera `build_host_win/a26run.exe` (MSVC, sem WSL).
2. `sh tools/host_runner/golden.sh <pasta_roms>` precisa imprimir `GOLDEN_OK`
   para mudanças que não devem alterar a emulação (otimizações). ROMs:
   `enduro.a26`, `pitfall.a26`, `riverraid.a26`, `freeway.a26`, `spaceinv.a26`,
   `combat.a26`, copiadas do SD. Só regrave (`record`) após conferir os PNGs.
3. `a26run <rom> --input "60-65:reset;100-699:fire;300-699:left" --png-at N`
   gera imagens; `A26_RAM=1` grava os 128 bytes de RAM por frame no CSV, o que
   permite comparar o estado do jogo entre inputs ou com o core de referência.
4. Compare sempre a RAM do jogo, não só a imagem: no Enduro a cena se mexia com
   as curvas e parecia responder ao joystick quando não respondia.

## 18. Modos de escala do submenu Image e overlay de FPS no blitter (19/10/2026)

**Validado em hardware pelo usuário** (escala e overlay de FPS aprovados). Arquivo: `pico_shared/drivers/ili9341/ili9341.c`.

### Escala: Fit, Stretch e Integer não mudavam nada

**Sintoma:** alternar `Scale` no submenu `Image >` não alterava a imagem.

**Causa:** o submenu (`a26_image_submenu`) estava correto. O blitter do Core 1 só
consultava `s_scale_mode` para `SCALE_INTEGER`, então **Fit e Stretch caíam no mesmo
ramo** (160 -> 320 em 2x horizontal; 160 -> 266 em GB_MODE). A vertical era fixa:
janela de 192 linhas em y=24..215 em qualquer modo. Além disso,
`ili9341_set_scale_mode()` chamava `nes_prepare_band_xmap()`, herança do NES cuja
tabela o blitter do A26 nunca usou, e não limpava a tela.

**Semântica atual** (fonte 160x192):

| Modo | Janela | Conteúdo |
| :--- | :--- | :--- |
| Fit | y=24..215 (192 linhas) | 2x horizontal (320, ou 266 em GB_MODE) |
| Stretch | y=0..239 (240 linhas) | mesmo mapa horizontal; vertical `src_y = row*192/240` |
| Integer | y=24..215 | 160 px 1x centralizado (x=80; x=98 em GB_MODE) |

- Ao mudar o modo (ou GB_MODE), o Core 1 limpa os 320x240 em preto uma vez
  (`s_last_geom`), para não deixar resto de Stretch nas faixas de Fit/Integer.
- `s_scale_mode` passou a `volatile` (lido pelos dois cores).
- Scanlines e HUD de volume usam a linha real da tela (`scr_y`), não mais `y + 24`.

### Overlay de FPS (START + A) carimbado no blitter, como no PicoGBC

O desenho anterior rodava no Core 0 e precisava de handshake de SPI0 com o Core 1.
Agora segue o `mk_ili9225_write_pixels()` do PicoGBC: o blitter carimba os glifos 8x8
(até 3 dígitos, branco sobre preto) em `line_packed` nas linhas **8..15 da janela**,
antes de cada linha ir ao SPI. Posição x=8 (x=53 em `GB_MODE=1`, fora da borda preta).

- `ili9341_set_fps_overlay()` só atualiza o texto e o flag; o Core 0 não toca no SPI.
- Removidos o buffer de overlay de 15 KB, a espera do blitter e `a26_spi_lock/unlock`.
  O handshake `s_spi_ovl_request/idle` ainda é servido no topo do laço do Core 1, sem uso atual.
- Desligar não deixa resíduo: o próximo frame sobrescreve a linha.
- **Mudança de posição:** o overlay deixa de usar as faixas pretas (linhas 0..23 e
  216..239) e passa a ficar sobre a imagem (tela 32..39 em Fit/Integer; 8..15 em Stretch).
  Isso substitui a descrição das seções 7c e 10 neste ponto.

### Binários
`PicoA26.uf2` 494.080 B (HW15), `PicoA26_hw16.uf2` 504.832 B (HW16), sem warnings.

### Pendente
Conferir em GB_MODE=1 as três escalas e o overlay de FPS (a validação citada acima
foi dada de forma geral).

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
- `ili9341_write_rect_565()` no driver. O `audio_i2s_flush()` ja existia neste driver
  (foi o modelo copiado para os outros).
- Descritor: `audio_free/push` (anel EXT 44,1 kHz), `flush_saves = NULL` (nao ha save
  de bateria), `audio_restore = a26_audio_restore` (flush + 44,1 kHz).
- ROM em RAM: nada de flash no resume.
- `_R` carregado dentro do loop, depois do 1o quadro, pela sequencia do Load State
  do menu de pausa (`s_resume_pending`).

### Correcao: chave e indicador de bateria sem efeito
O codigo era identico ao do PicoNES (mesma placa RP2350B no ELF, mesma cadeia
`dmg_read_ui_buttons -> dmg_power_poll`). Resolvido com build limpo do HW16:
`build_core2350b.ps1` configura com `-DPICO_BOARD=waveshare_core2350b` explicito,
para nenhum cache antigo de RP2350A (30 GPIOs, ADC 26..29) sobreviver. Os UF2 que
vao para o cartao sao os de `PicoA26/build_hw16`.

### Status
Validado em hardware (Core2350B) pelo usuario em 2026-11-05: OFF no jogo, no menu
de pausa e no seletor; volta no mesmo ponto, com som e FPS normais.
