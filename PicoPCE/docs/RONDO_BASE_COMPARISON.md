# Rondo: comparação com o projeto base

Base lida em 2026-10-19: [pico-pcePlus a76d8e895cd1fda63ffce3bfe52e91f80a5018fe](https://github.com/PicoPlus-devel/pico-pcePlus/tree/a76d8e895cd1fda63ffce3bfe52e91f80a5018fe).
Submódulo da base: [pico_shared 6e65df4a54b916e26a67edadfd0833c6b940e0fa](https://github.com/PicoPlus-devel/pico_shared/tree/6e65df4a54b916e26a67edadfd0833c6b940e0fa).
Os arquivos foram baixados nessas revisões fixas para `build_hw16_diag/upstream_base/` e comparados com o código local.

| Caminho | Resultado da comparação |
|---|---|
| `pce-go/h6280.c`, `h6280_instr.h` | Iguais à base, normalizando apenas finais de linha. |
| `pce-go/pce.h` | Igual à base; os acessos inline à memória são os mesmos. |
| `pce-go/gfx.c` | Era igual à base na comparação inicial. Após a captura R3, recebeu a correção por célula de sprite descrita abaixo. |
| `pce-go/pce.c` | Apenas instrumentação adicional de estágio e DMA. A leitura da origem de DMA sem bounds também existe na base; não é uma divergência identificada do port. |
| `pce-go/cd.c` | Decodificação ADPCM, IRQs, mapeamento CD/SCD RAM e máquina de estados SCSI iguais. Diferenças: instrumentação, varredura de BIOS somente em `/bios/` e tamanho do buffer dessa varredura. A restrição a `/bios/` é uma preferência anterior do usuário. |
| `pce-go/pce-go.c` | Mudanças de HuCard/cache e códigos de erro do carregamento; as alocações e a ligação da memória de CD no LoadDisc seguem a base. |
| `pce-go/cd_chd.c` | Cache local de 32 entradas contra 4 na base; diferenças no tratamento/probe de pregap já existentes no port. Esses caminhos não explicam sozinhos a reprodução anterior em CUE/BIN. Não foram alterados nesta investigação. |
| `pico_shared/.../SetupPsram.c`, `PicoPlusPsram.cpp` | Iguais aos da revisão do submódulo usada na base. |
| `pico_shared/.../audio_i2s.c` | Função adicional de envio em lote no port. O driver original permanece presente. |
| `main.cpp` | Integração diferente: framebuffer ILI9341/ST7789 no Core 1, envio de áudio I2S em lote, prefetch CD no Core 0, pacing próprio, menus e loaders locais. |

## Evidência recebida no aparelho (R2)

HardFault no Core 0, quadro 6447, `CFSR=0x00008200`, `HFSR=0x40000000`, `BFAR=0x000735A0`.
Isso indica BusFault preciso, com BFAR válido, escalado para HardFault. O MMFAR mostrado não é válido porque MMARVALID não está ativo.
`C1: CORE1 DISPLAY` é o último marcador de execução do outro núcleo, não uma evidência de que ele falhou.

O R2 mostrou `PC=0`, `LR=0x112A4898`; nenhum deles pôde ser localizado no ELF correspondente.
Foi encontrado um erro confirmado no leitor do diagnóstico: somava 18 palavras ao SP para ler PC/LR quando havia armazenamento de ponto flutuante. O teste antigo repetia essa hipótese incorreta.
Na [figura 2-6 do manual oficial Cortex-M33](https://documentation-service.arm.com/static/5f16e93d20b7cf4bc524af1d#page=52), R0–xPSR são as primeiras oito palavras também nesse formato; os registradores FP vêm depois. Portanto, PC fica em SP+0x18 e LR em SP+0x14.

Foi criado primeiro um teste com esse layout oficial, que falhou no código R2. A correção R3 lê os offsets corretos, registra o estágio anterior à falha e copia R0–R3/R12/LR/PC/xPSR. EXC_RETURN e SP também ficam visíveis. Contexto adicional de segurança não suportado é rejeitado explicitamente.

## Captura R3: instrução localizada e corrupção reproduzida

Na captura física R3: `PC=100D6E86`, `LR=1008E677`, `R0=R12=000735A0`,
`R1=1129F02C`, `R2=0000031C`, `CFSR=00008200`, `BFAR=000735A0`, Core 0,
6153 quadros. O último estágio anterior ao HardFault era CD AUDIO.

O ELF R3 preservado resolve PC para `memcpy` e LR para
`cd_audio_generate_samples`, `pce-go/cd.c:2316`. A instrução é `str r3,[r0]`,
escrita no destino do áudio, não leitura do setor de origem em PSRAM.

O mesmo ELF põe `cd_audio_buffer` em 0x2001B084 e a linha visível em 0x2001B0A4,
32 bytes depois. O buffer físico de linha reserva apenas 16 bytes à esquerda.
O desenhador de sprites filtrava a largura total, mas ainda desenhava a célula
invisível de um sprite de 32 pixels parcialmente fora da borda esquerda. Em
linhas posteriores à primeira, os limites de framebuffer virtuais permitiam a
escrita antes do buffer físico, sobre os globais vizinhos.

`tests/render/test_sprite_bounds.c` executa o renderizador real com esse layout.
Um sprite de 32 pixels em x=-29 altera um ponteiro 200735A0 para 000735A0 antes
da correção, reproduzindo exatamente o destino inválido da captura.

A correção pula cada célula de 16 pixels inteiramente fora da janela antes de
formar o ponteiro de escrita. O teste de corrupção agora passa, e um decodificador
independente de bitplanes confirma os pixels visíveis em 119.808 combinações de
posições, tamanhos, alturas e espelhamentos. Guardas de memória e sanitizadores
também estão ativos no teste. IRQs, ADPCM, cache CHD e clocks não foram alterados.

O UF2 separado usado na validação é `PicoPCE_hw16_rondo_fix.uf2`, com
ELF/map/hashes próprios, reproduzível com `build_core2350b.ps1 -RondoFix`.
O usuário aprovou a correção no aparelho em 2026-10-19 e pediu para manter o
relatório de falhas no firmware principal. A correção e o diagnóstico agora
integram os builds principais HW15/HW16. A aprovação não registra uma contagem
específica de passagens nem confirma individualmente toda a matriz de testes.
