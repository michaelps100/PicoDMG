# Arquitetura e Integração do Display SPI (ILI9341/ST7789) e Botões PicoDMG

## 1. Visão Geral da Arquitetura

O projeto `pico-bootLoader` foi adaptado para suportar **modo de saída dual**, controlado pela flag de compilação `-DUSE_ILI9341=1` ou `0`:

- **Modo DVI / HDMI (`-DUSE_ILI9341=0`)**: Mantém a saída de vídeo digital original (PicoDVI / HSTX) com renderização por scanline dirigida pelo Core 1, adequada para uso conectado a monitores/TVs (`HW_CONFIG=2` padrão Waveshare RP2350 Plus).
- **Modo Display SPI Autônomo (`-DUSE_ILI9341=1`)**: Desativa o pipeline DVI do Core 1 e direciona o desenho para um framebuffer em SRAM de 320x240 pixels (RGB565), transmitido diretamente via hardware SPI + DMA para telas TFT SPI (ILI9341 ou ST7789 no padrão do projeto [dodger-one/PicoDMG](https://github.com/dodger-one/PicoDMG)).

---

## 2. Pinos e Hardware (`HW_CONFIG=15` para PicoDMG)

Quando `-DUSE_ILI9341=1` está ativo, o CMake define por padrão a configuração `HW_CONFIG=15`:

### Display SPI (SPI0)
- **CS**: GPIO 17
- **SCK**: GPIO 18
- **MOSI**: GPIO 19
- **DC**: GPIO 20
- **RST**: GPIO 21
- **BL (Backlight)**: GPIO 22

### Cartão MicroSD (SPI1)
- **MISO**: GPIO 12
- **CS**: GPIO 13
- **SCK**: GPIO 14
- **MOSI**: GPIO 15

### Mapeamento Direto dos Botões Físicos PicoDMG
Em `main.cpp`, a função `readPads()` lê as GPIOs diretamente com pull-up interno e nível lógico baixo ativo:
- **GP2**: Direcional Cima (UP)
- **GP3**: Direcional Baixo (DOWN)
- **GP4**: Direcional Esquerda (LEFT)
- **GP5**: Direcional Direita (RIGHT)
- **GP6**: Botão A
- **GP7**: Botão B
- **GP8**: Botão SELECT
- **GP9**: Botão START

---

## 3. Resolução do Pipeline Gráfico e Driver SPI

### Framebuffer e Flush
- O driver `src/drivers/ili9341/ili9341.c` aloca um buffer alinhado `s_fb` de 320x240 pixels.
- `Frens::framebuffer` é configurado para apontar para `ili9341_get_framebuffer()`.
- A interface (`src/gui.cpp`), o menu (`pico_shared/menu.cpp`) e o protetor de tela (`src/screensaver.cpp`) desenham diretamente nas linhas desse framebuffer e chamam `ili9341_flush(true)` ao final de cada frame renderizado.

### Correção de Duplicação e Truncamento de Linhas (16-bit SPI DMA)
- **Problema anterior**: `spi_init()` inicializa o controlador PL022 do RP2040/RP2350 em modo de 8 bits. Ao fazer transferência DMA de 16 bits para `dr`, os 8 bits superiores de cada pixel eram ignorados, enviando apenas 320 bytes por scanline em vez de 640 bytes. Isso dividia a tela em 4 quadrantes (duas cópias da imagem na metade superior e linhas não inicializadas na inferior).
- **Correção implementada**:
  - Antes de iniciar a transferência DMA das linhas, `ili9341_flush()` comuta dinamicamente o formato do SPI para 16 bits:
    ```c
    spi_set_format(ILI9341_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    ```
  - Cada transferência DMA envia os 16 bits completos de cada pixel RGB565.
  - Ao término da linha 239, o driver aguarda o shift register drenar (`while (spi_is_busy(ILI9341_SPI))`), limpa a FIFO de RX e restaura o SPI para 8 bits para que comandos e janelas de desenho funcionem corretamente:
    ```c
    spi_set_format(ILI9341_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    ```

---

## 4. Correções de Build e Compatibilidade

1. **Restauração do `CMakeLists.txt`**:
   - Reintegrados subdiretórios `pico_shared`, `dvi`, `util`, `xinput_host`, `src/drivers`.
   - Linkadas bibliotecas essenciais (`cmsis_core`, `pico_multicore`, `hardware_dma`, `hardware_pio`, `pico_shared`).
   - Mantidos `frens_link_as_bootloader` e `pico_add_extra_outputs` para geração do `.uf2`.
2. **CMSIS Cortex-M33**:
   - Restaurado `#include "RP2350.h"` em `src/app_launch.c`.
3. **Compatibilidade com GCC 15 (ARM)**:
   - Adicionada a flag `-Wno-incompatible-pointer-types` para permitir a compilação do driver TinyUSB `xinput_host.c` no toolchain GCC 15.2.1.
4. **Isolamento DVI**:
   - Recursos exclusivos de DVI no Core 1 (`dvi_`, `exclProc_`, buffer de áudio DVI em `wavplayer.cpp`) foram devidamente protegidos por `#if !USE_ILI9341`.

---

---

## 5. Leitura Direta de Arquivos `.uf2` em `/emu/` (Sem Dependência de `emulators.txt`)

O bootloader foi atualizado para listar diretamente todos os arquivos `.uf2` presentes no cartão MicroSD:

1. **Varredura Direta no Diretório `/emu/` (Exclusão de Pastas)**:
   - Inicialmente verifica se existe o subdiretório específico da placa (`<BASEDIR>/<HW_CONFIG>`, ex: `/emu/15`).
   - Se o subdiretório não existir ou não contiver arquivos `.uf2`, o bootloader faz **fallback automático diretamente para o diretório raiz `/emu/`**.
   - **Filtro estrito de arquivos**: Subdiretórios (`entries[i].IsDirectory`) e arquivos com extensões diferentes de `.uf2` são completamente ignorados. Apenas arquivos `.uf2` válidos são adicionados à lista de emuladores e exibidos no menu.

2. **Independência de `emulators.txt`**:
   - O arquivo `emulators.txt` não é mais obrigatório e sua ausência **não gera erro fatal**.
   - Se `emulators.txt` estiver presente, ele é usado de forma opcional para fornecer nomes amigáveis (`display_name`) e chaves de arte gráfica de temas (`image_key`).
   - Se `emulators.txt` não existir (ou se o `.uf2` não estiver listado nele), o bootloader deriva o nome e o rótulo diretamente do `program_name` (extraído do cabeçalho `binary_info`) ou do nome do arquivo (ex: `nes.uf2` -> `nes`), exibindo-o perfeitamente no menu sem descartá-lo.

3. **Fallback Visual e Modo de Exibição Inicial**:
   - Quando não há temas de arte gráfica no cartão (`themes_count() == 0`), o bootloader **inicia por padrão em Modo Texto**, exibindo a lista vertical completa de emuladores na tela de imediato.
   - No **Modo Gráfico**, caso o emulador não possua imagem PNG correspondente no cartão, o bootloader renderiza um cartão visual contendo o nome do aplicativo, nome do arquivo `.uf2`, contador `[ X de Y ]` e status de gravação na flash em vez de exibir tela preta.
   - Pressionar o botão **SELECT** continua alternando a qualquer momento entre o Modo Texto (lista) e o Modo Gráfico (carrossel).

---

## 6. Arquitetura de Gravação na Flash e Requisitos dos Arquivos `.uf2`

1. **Gravação Sob Demanda (Botão A)**:
   - Os arquivos `.uf2` no cartão MicroSD **não são gravados automaticamente** na Flash ao ligar o console. Eles formam a biblioteca de emuladores disponíveis.
   - Para gravar e iniciar um emulador, selecione-o na lista e pressione o **Botão A** (GP6).
   - O bootloader grava apenas o emulador selecionado na partição de aplicativos da flash e executa-o imediatamente.

2. **Offset Obrigatório de Memória (`0x10080000`)**:
   - O `pico-bootLoader` reside permanentemente nos primeiros 512 KB da memória Flash (`0x10000000` a `0x10080000`).
   - Binários `.uf2` convencionais de Raspberry Pi Pico são compilados para o endereço base `0x10000000`. Se o bootloader gravasse um arquivo desse tipo, **ele sobrescreveria a si mesmo**.
   - O bootloader executa uma checagem rigorosa (*pre-flight validation*): arquivos `.uf2` com blocos abaixo de `0x10080000` são **rejeitados com aviso na tela** (`WRONG_LINK_ADDR`).
   - Para um emulador ser gravável pelo bootloader, ele deve ser compilado com:
     `-DBUILD_FOR_BOOTLOADER=ON` (usando `frens_offset_for_bootloader()` para realocar o código em `0x10080000`).

3. **Feedback Visual em Display SPI (`USE_ILI9341=1`)**:
   - O pipeline de gravação mantém a tela ligada no display SPI, exibindo a mensagem `"Flashing..."` e a barra de progresso em tempo real atualizada via `ili9341_flush_rows(158, 192)` a cada avanço de 2% gravado, sem degradar a velocidade de gravação.

---

## 7. Comandos de Compilação

### Gerar binário para Display SPI ILI9341 / ST7789 + Botões PicoDMG
```powershell
$env:PATH = "C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"
cmake -B build_ili9341 -G Ninja -DUSE_ILI9341=1
ninja -C build_ili9341
```
*Saída gerada:* `build_ili9341/pico-bootLoader.uf2`

### Gerar binário original para DVI / HDMI
```powershell
$env:PATH = "C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"
cmake -B build_dvi -G Ninja -DUSE_ILI9341=0
ninja -C build_dvi
```
*Saída gerada:* `build_dvi/pico-bootLoader.uf2`

---

## 8. Otimizações Extremas de Velocidade no Carregamento Flash

Para solucionar de forma definitiva o tempo de espera do carregamento, aprofundamos a análise e eliminamos todos os gargalos do pipeline:

1. **Flush Parcial de Linhas Sujas no Display SPI (`ili9341_flush_rows(158, 192)`)**:
   - **Gargalo descoberto**: Transmitir a tela inteira de 320x240 pixels (153.600 bytes) via SPI DMA a cada incremento de porcentagem consumia mais de **50 ms por quadro**. Redesenhar a tela 100 vezes consumia **mais de 5 segundos** parado unicamente esperando a transmissão do display!
   - **Solução implementada**: Implementada a função `ili9341_flush_rows(int y_start, int y_end)`. Como a barra de progresso e seu rótulo numérico ocupam estritamente as linhas verticais 160 a 190, agora apenas as 35 linhas sujas (158 a 192) são enviadas via SPI. Cada quadro caiu de **50 ms para ~4 ms** (uma economia direta de quase 5 segundos de espera).
   - O throttling agora atualiza a barra a cada **2%** (50 passos fluidos), totalizando apenas ~0,2 s de display em toda a gravação.

2. **Buffer de Leitura MicroSD Ampliado para 32 KB (64 Blocos / `CMD18`)**:
   - Expandido o buffer de streaming em `src/uf2_loader.c` para 64 blocos UF2 (32 KB). Uma imagem inteira de 2 MB agora é lida em apenas 32 rajadas contínuas do barramento SPI.

3. **Buffer de Escrita Flash Ampliado para 16 KB (64 Páginas)**:
   - Em vez de acionar a Bootrom a cada 4 KB, agrupamos 16 KB (64 páginas de 256 bytes) de dados sequenciais em `prog_buf`. A gravação é realizada em rajadas contínuas em modo QSPI pelo hardware.

4. **Frequência Rápida do SPI do Cartão SD (`CLK_FAST_DEFAULT`)**:
   - Em `pico_shared/FrensHelpers.cpp`, o clock do periférico SPI de hardware para o MicroSD foi elevado para a velocidade nativa rápida (`CLK_FAST_DEFAULT` = até 50 MHz, ajustado pelo divisor do RP2350 para 37,5 / 25 MHz), acelerando a taxa de transferência de leitura.

5. **Redução da Latência de Confirmação (`FLASH_NOTICE_MS = 50`)**:
   - Reduzido o atraso artificial de 500 ms para 50 ms antes de iniciar a gravação após o pressionamento do botão A.

---

## 9. Artes `.565` no Modo Gráfico (Tema)

### Formato
- Arquivo `<chave>.565` em `0:/emu/assets/themes/<N>/`.
- Cabeçalho de 4 bytes: `uint16 largura`, `uint16 altura` (little-endian), seguido de `largura × altura` pixels **RGB565 little-endian**, linha a linha.
- Tamanho padrão **266×240** = exatamente a janela do `GB_MODE=1` (x=45..310). Qualquer tamanho até 320×240 é aceito e centralizado na área visível (janela DMG com `GB_MODE=1`, tela cheia com `GB_MODE=0`), com preto em volta.

### Busca da arte (`src/gui.cpp`, `main.cpp`)
- Em cada pasta de tema (ativo, depois tema 0), `.565` é tentado **antes** do `.444` legado. Só builds `USE_ILI9341`.
- Ordem das chaves, primeira que existir vence: **nome do `.uf2` sem extensão** (`PicoNES.uf2` → `PicoNES.565`), depois o `image_key` do `emulators.txt` (ou derivado), depois o nome do programa.
- `.444` legado continua funcionando: é convertido para RGB565 na carga, para todos os buffers do modo gráfico terem o mesmo formato.

### Pipeline de cor
- O framebuffer do bootloader é RGB444 (`0x0RGB`) e o flush converte para RGB565. No modo gráfico os buffers já estão em RGB565, então o driver tem um modo de passagem direta: `ili9341_set_native_565(true)` (ligado só em volta do flush de cada quadro do modo gráfico, desligado logo depois).
- Cores de sobreposição (rodapé, HUD de volume, cartão "sem arte") passam por `gui_color()` (RGB444 → RGB565) quando o modo de passagem está ativo.

### Conversão
- `tools/img_to_565.ps1` (só PowerShell + System.Drawing, sem instalar nada): redimensiona com bicúbica de alta qualidade, cobrindo o alvo e cortando o excesso centralizado (proporção mantida).
  ```powershell
  powershell -File tools\img_to_565.ps1 -InDir C:\dev\PicoDMG-Temp\NewIMG -OutDir I:\emu\assets\themes\0
  ```

---

## 10. Transição entre Emuladores por Rolagem de Hardware

### Problema
Cada quadro da transição recompunha e reenviava a tela inteira (153 KB, ~25 ms) em 16 quadros, depois de ler a próxima imagem do SD. Como o envio era mais lento que a varredura do painel, o corte caía numa linha diferente a cada quadro (efeito "cascata").

### Solução (`ili9341_hscroll_*`, `gui_hw_slide_*`)
- ST7789 e ILI9341 rolam ao longo do eixo de 320 px (gates), que em paisagem (`MADCTL` com `MV`) é o **eixo X da tela**. Usa `VSCRDEF` (0x33) e `VSCRSADD` (0x37); saída do modo com `NORON`.
- A imagem antiga fica parada na GRAM; a cada passo só muda o endereço de leitura e são enviadas **apenas as colunas reveladas** (`ili9341_write_cols_565`, buffer de linha duplo com DMA).
- `GB_MODE=1`: áreas fixas `TFA`/`BFA` mantêm as bordas pretas (45 px / 9 px); só a janela 266 px rola.
- `MY` ligado (`ROTATE=1`: 0xE8 / 0xA0) inverte as áreas fixas e o sinal do deslocamento; tratado em `ili9341_hscroll_begin`.
- Retrato ou `GRAM offset` ≠ 0: `begin()` retorna falso e a troca é instantânea.
- Duração 220 ms (`GUI_HW_SLIDE_US`), ease-out cúbico baseado em tempo. Roda até o fim num laço próprio, para nada tocar no painel em modo de rolagem.
- O rodapé de botões rola junto (a rolagem move colunas inteiras) e volta ao lugar no fim.

### Cache na PSRAM
- Uma imagem 320×240 decodificada por emulador (até 32), alocada sob demanda só com PSRAM e com folga de 512 KB (`f_malloc` entra em `panic()` quando falha).
- Vizinhos (`sel±1`) pré-carregados com o menu parado e sem botão pressionado; troca de tema invalida o cache.
- Sem PSRAM (HW15): sem cache, cada troca lê do SD; a rolagem por hardware funciona igual (precisa de um só buffer).

---

## 11. Abertura ao Ligar (Logo + Som) — substitui a tela de logo + QR

### Comportamento (`showBootIntro`, `main.cpp`)
- Tela preta → 250 ms → logo surge do preto (fade-in de 350 ms) e desce em velocidade constante até o centro da área visível em 1150 ms → o som toca ao pousar → logo parado 1300 ms (e até o som terminar) → busca de emuladores com o logo ainda na tela → lista.
- Qualquer botão pula. A espera pela soltura acontece **depois** da checagem do calibrador, para SELECT segurado ao ligar continuar abrindo o calibrador; SELECT+START+UP segue indo para o BOOTSEL.
- **Só ao iniciar o console** (energizar ou acordar do standby do próprio bootloader, que reinicia sem marca). **Acordar do standby de um emulador** (`scratch[5]` com `0x5E5A6Axx`, §45 do `AGENTS.md` raiz) pula a abertura e o init do display e salta direto para o app, que toca a mesma abertura no Core 1 enquanto recarrega o jogo e o savestate. Ao voltar de um emulador (`returnRequested`, `scratch[7] = LOADER_RETURN_MAGIC`) a abertura é pulada e a tela fica preta durante a busca.

### Assets (`src/boot_anim_assets.h`, gerado — não editar à mão)
- Logo 150×39 em RGB565, alfa já misturado sobre preto (tamanho nativo, sem escala).
- Som: mono 16 bits 22050 Hz, ~0,7 s, cortado do áudio do vídeo de referência do início do ataque até o fim do decaimento, com fade de 40 ms no final.
- Gerador: `tools/make_boot_anim_assets.py logo565.raw 150 39 audio.s16 src/boot_anim_assets.h`. Logo fonte em `tools/boot_anim_src/logo.png`.

### Áudio I2S no bootloader
- `pico_shared/BoardConfigs.cmake`, HW_CONFIG 15 e 16: `USE_I2S_AUDIO=PICO_AUDIO_I2S_DRIVER_PCM5000A`, MAX98357A em **GP26 (DIN) / GP27 (BCK) / GP28 (LRCK)**, PIO0 — mesma ligação dos emuladores. Antes estava desligado (`0`).
- Reprodução: reamostragem 22,05 → 44,1 kHz por interpolação linear, completando o anel de 1024 quadros (`EXT_AUDIO_GET_FREE`) a cada volta do laço, seguida de 50 ms de silêncio para o amplificador não parar numa amostra ≠ 0.
- Volume segue `VOLUME=` do `/boot.txt`: ganhos `{0, 40, 80, 130, 190, 256}/256`; `VOLUME=0` = mudo.
- `pico_shared/wavplayer.cpp`: ramos que usam o anel DVI (`dvi_`) isolados com `#if !USE_ILI9341`, porque deixavam de compilar com áudio externo ligado.
- **Antes de pular para o app** (`app_launch_run`, `src/app_launch.c`): `reset_block(PIO0 | DMA)`, para o estado do I2S da abertura não passar para o emulador, que espera configurar tudo do zero como num boot a frio.

### Tearing na descida
- Em paisagem o painel varre ao longo do eixo X; escrever o logo linha a linha fazia a varredura cortá-lo na diagonal.
- `ili9341_write_rect_by_columns()`: envia só as 150 colunas do logo, **coluna por coluna** (eixo da varredura). No pior caso sobra uma emenda vertical de 1 px.
- Sem o pino TE ligado a um GPIO não há sincronismo com a varredura; seria o próximo passo se for preciso eliminar por completo.

---

## 12. Painel: Taxa de Varredura e Limpeza da GRAM

- **ST7789 a ~111 Hz**: `st7789_init_sequence()` agora envia `PORCTRL 0xB2 = {0x0C,0x0C,0x00,0x33,0x33}` e `FRCTRL2 0xC6 = 0x01` (padrão de fábrica 0x0F = 60 Hz) — os mesmos valores de `dmg_panel_st7789_timing()` do `shared_menu`, já validados no Core2350B. Comandos exclusivos do ST7789: não enviar ao ILI9341.
- **ILI9341 inalterado**: o `FRMCTR1` nunca foi medido no vidro (ver §26 do `AGENTS.md` raiz).
- **`clear_gram()` antes do `DISPON`** nos dois init: nem o reset por hardware nem o `SWRESET` limpam a GRAM, e o standby da chave só manda `SLPIN` sem cortar a energia. Sem isso, ao religar a chave o painel acendia com o último quadro da lista até a abertura pintar por cima. Custa ~40 ms no boot (30 MHz).

---

## 13. Scripts de Build Atuais

| Placa | Script | Pasta | Saída |
| :--- | :--- | :--- | :--- |
| **HW15** (RP2350 Plus / ILI9341) | `build_ili9341.ps1` | `build_hw15/` | `build_hw15/pico-bootLoader.uf2` |
| **HW16** (Core2350B / ST7789) | `build_core2350b.ps1` | `build_hw16/` | `build_hw16/pico-bootLoader.uf2` |

Os dois configuram explicitamente (`HW_CONFIG`, `USE_ILI9341`, painel padrão). Variáveis `CACHE` do `BoardConfigs.cmake` não sobrescrevem um cache existente: ao mudar uma delas (ex.: `USE_I2S_AUDIO`), apague o `CMakeCache.txt` da pasta antes de compilar. Com a abertura, a imagem do bootloader termina em ~0x10056270 (~345 KB dos 512 KB reservados).

---

## 📋 Status: Concluído e Validado
- Ambos os targets (`USE_ILI9341=1` e `USE_ILI9341=0`) compilam sem erros e geram seus respectivos arquivos `.uf2`.
- O layout de tela de 320x240 pixels é transmitido na integridade para o display SPI via DMA em 16 bits.
- Os arquivos `.uf2` colocados diretamente em `/emu/` são detectados e exibidos no menu sem a necessidade do arquivo `emulators.txt`.
- Validado no console Core2350B (HW16): artes `.565` no modo gráfico, transição por rolagem de hardware, descida do logo sem tearing perceptível, abertura pulada ao voltar de emulador, sem quadro antigo ao religar a chave.
- Pendente de validação em hardware: som da abertura (nível conforme `VOLUME=`); som de todos os emuladores depois do `reset_block(PIO0 | DMA)` no salto para o app; HW15 (RP2350 Plus / ILI9341) com as mesmas mudanças.
- Gravação ultra-rápida com `ili9341_flush_rows()`, buffer de SD de 32 KB, gravação Flash em blocos de 16 KB, apagamento de 64 KB e clock acelerado no barramento SPI.