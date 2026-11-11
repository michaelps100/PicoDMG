# Build script for PicoLINX targeting the Waveshare Core2350B (RP2350B, 16 MB flash + 8 MB PSRAM)
# with the ST7789 panel, linked for pico-bootLoader.
#
# Differences from build_for_bootloader.ps1 (which stays on the ILI9341 HW15 board):
#   -DPICO_BOARD=core2350b        -> boards/core2350b.h (PSRAM CS on GPIO 47, LED on 39)
#   -DDEFAULT_PANEL_ST7789=1      -> fallback panel when the card has no /boot.txt
#
# The panel itself is still a runtime decision: shared_menu/dmg_panel.c reads
# ST7789= / MADCTL= / ROTATE= / INVERT= from /boot.txt. DEFAULT_PANEL_ST7789 only
# covers a card WITHOUT /boot.txt -- with a /boot.txt present but no ST7789= key,
# dmg_panel falls back to this value, which is why the console build sets it to 1.

$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_core2350b"

Write-Host "`n[1/3] Configurando CMake para RP2350B + PSRAM + ST7789 + Bootloader..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=core2350b `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DDEFAULT_PANEL_ST7789=1 `
  -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { Write-Host "[ERRO] cmake falhou (exit $LASTEXITCODE)" -ForegroundColor Red; exit 1 }

Write-Host "`n[2/3] Compilando PicoLINX (Core2350B)..." -ForegroundColor Cyan
ninja -C $BuildDir lynx
if ($LASTEXITCODE -ne 0) { Write-Host "[ERRO] ninja falhou (exit $LASTEXITCODE). Nenhum binario gerado." -ForegroundColor Red; exit 1 }

Write-Host "`n[3/3] Verificando e copiando binarios UF2..." -ForegroundColor Cyan

$PicoUf2 = ".\bin\core2350b\Release\PicoLNX.uf2"
# A failed ninja leaves the previous .uf2 in place, so Test-Path alone can copy a
# stale binary and still print [OK]. Refuse anything older than 5 minutes.
if (Test-Path $PicoUf2) {
    $AgeMin = ((Get-Date) - (Get-Item $PicoUf2).LastWriteTime).TotalMinutes
    if ($AgeMin -gt 5) {
        Write-Host "[ERRO] PicoLNX.uf2 tem $($AgeMin.ToString('F0')) min -- binario obsoleto, recusando copiar." -ForegroundColor Red
        exit 1
    }
    Copy-Item $PicoUf2 -Destination ".\PicoLNX_hw16.uf2" -Force
    # Segunda copia dentro do diretorio de build, para nao ter de garimpar
    # bin/core2350b/Release/ quando se procura o artefato da placa.
    Copy-Item $PicoUf2 -Destination ".\$BuildDir\PicoLNX.uf2" -Force
    $PicoSize = (Get-Item $PicoUf2).Length
    Write-Host "[OK] PicoLNX_hw16.uf2 gerado com sucesso: $PicoSize bytes" -ForegroundColor Green
    Write-Host "     Copiado tambem para .\build_core2350b\PicoLNX.uf2" -ForegroundColor Gray
    Write-Host "     Cartas: /roms/lynx/ (jogos) e /saves/lynx/ (saves/states/cfg)" -ForegroundColor Gray
} else {
    Write-Host "[ERRO] PicoLNX.uf2 nao encontrado!" -ForegroundColor Red
    exit 1
}

Write-Host "`nArquivo pronto na raiz do projeto:" -ForegroundColor Cyan
Write-Host "  -> .\PicoLNX_hw16.uf2" -ForegroundColor Green
Write-Host "  -> .\$BuildDir\PicoLNX.uf2" -ForegroundColor Green
Write-Host "`nCopie para /emu/16/ no cartao MicroSD. Defina ST7789=1 em /boot.txt se usar outro painel." -ForegroundColor Yellow
