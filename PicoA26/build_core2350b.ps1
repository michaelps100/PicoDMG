# Build PicoA26 for the Waveshare Core2350B console (HW_CONFIG 16).
#
# The panel (ILI9341 or ST7789) is not chosen here: shared_menu/dmg_panel.c reads
# ST7789= from /boot.txt at runtime, and DEFAULT_PANEL_ST7789=1 only covers the
# case where the card has no /boot.txt. HW_CONFIG 16 sets that default anyway; it
# is passed explicitly below so the build does not depend on that coupling.
#
# The board's 8 MB of PSRAM is deliberately NOT used for the cartridge: the 2600's
# largest common ROM is 64 KB and already runs zero-copy in the s_cart_rom SRAM
# buffer, so there is nothing to gain. This target exists for the ST7789 panel.

$ErrorActionPreference = "Stop"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_hw16"

Write-Host "Configurando CMake para Waveshare Core2350B (HW_CONFIG 16) + TFT SPI0 + microSD SPI1..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_core2350b `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=16 `
  -DDEFAULT_PANEL_ST7789=1 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] CMake falhou; build cancelado." -ForegroundColor Red
    exit 1
}

Write-Host "Compilando PicoA26..." -ForegroundColor Cyan
ninja -C $BuildDir PicoA26

# Check the exit code, not just whether the file exists: a failed ninja leaves
# the previous build's .uf2 in place, so Test-Path alone copies a stale binary
# and reports success.
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] ninja falhou (exit $LASTEXITCODE). Nenhum binario gerado." -ForegroundColor Red
    exit 1
}

$Uf2 = "$BuildDir/PicoA26.uf2"
if (Test-Path $Uf2) {
    # A successful no-op Ninja build is valid; file age alone does not imply
    # stale firmware. CMake and Ninja exit codes were checked above.
    Copy-Item $Uf2 -Destination ".\PicoA26_hw16.uf2" -Force
    $Size = (Get-Item $Uf2).Length
    Write-Host "`n[SUCESSO] PicoA26_hw16.uf2 ($Size bytes)" -ForegroundColor Green
    Write-Host "Copie para o cartao MicroSD em /emu/16/PicoA26.uf2" -ForegroundColor Yellow
    Write-Host "Defina ST7789=1 em /boot.txt para o painel ST7789 do Core2350B." -ForegroundColor Yellow
} else {
    Write-Host "[ERRO] $Uf2 nao encontrado apos compilacao." -ForegroundColor Red
    exit 1
}