# PicoA26 1.3-region-r3 — 19/10/2026

Candidato para teste no RP2350. Compilação ARM nativa Windows, sem testes de
host ou WSL. Não há validação física desta versão.

Alterações:
- Paletas PAL e SECAM recuperadas dos dados do core original e convertidas
  para RGB565; relógio PAL 3546894 Hz separado do NTSC 3579545 Hz.
- Sem indicação regional no arquivo, oito frames válidos consecutivos com
  mais de 285 linhas selecionam PAL; frames menores selecionam NTSC. Frames
  incompletos ou atingindo o limite de segurança não participam da seleção.
- Tags NTSC, PAL, SECAM, (E)/(Europe), (U)/(USA) no nome são aproveitadas sem
  perguntar ao usuário. PAL60/NTSC50 preservam a paleta indicada e a duração
  realmente produzida pela ROM. SECAM e PAL60 sem metadados não podem ser
  distinguidos de PAL/NTSC apenas pela quantidade de linhas. PAL-M marcado
  usa a paleta e aproximação de clock NTSC.
- A altura visível medida no frame anterior é ajustada ao buffer existente de
  160x192. Frames PAL mais altos deixam de perder todas as linhas após a 192.
- No modo score, o playfield não encobre jogadores/mísseis; sua cor varia
  por metade da tela, enquanto a bola conserva COLUPF.
- Os 15 pares de colisão são acumulados por uma tabela de 64 entradas (128
  bytes SRAM). CXCLR e leituras dos oito registradores usam latches de 16 bits.
  Save/load sincroniza a representação serializada, preservando os pares.
- Leitura/escrita da RAM e leitura de ROMs 2K/4K da CPU evitam o decodificador
  geral. FE conserva a observação do barramento; demais acessos usam o decoder.
- Estados v6 acrescentam região, cores e altura capturada. States antigos são
  recusados sem alterar a partida. CPU permanece 280 MHz, sem PSRAM.

Compilações finais, ambas com exit 0:
| Alvo | UF2 (bytes) | RAM usada | Margem na região RAM |
|---|---:|---:|---:|
| HW15 | 486912 | 433468 | 90820 |
| HW16 | 497664 | 434356 | 89932 |

Logs: build_bootloader/region-r3-final-build.log e
build_hw16/region-r3-final-build.log. Offset de bootloader preservado.

Pendências físicas: FPS e áudio do Pitfall, imagem e controles do Enduro,
cores/altura em PAL/SECAM, menus e states. A falha de controle do Enduro não
teve causa demonstrada; esta versão não a registra como corrigida. Não houve
remapeamento especulativo do joystick.

Referências:
- https://github.com/DenizBasgoren/atari2600emu/blob/master/src/video.c
- https://github.com/DenizBasgoren/atari2600emu/blob/master/src/frontend.c
- https://github.com/stella-emu/stella/issues/901
- third_party/mcume_picovcs/UPSTREAM.txt