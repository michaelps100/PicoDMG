# Build PicoSNES for the Waveshare Core2350B console (HW_CONFIG 16, RP2350B +
# 8 MB PSRAM), targeting the pico-bootLoader partition at 0x10080000.
#
# The panel (ILI9341 or ST7789) is NOT chosen here: shared_menu/dmg_panel.c
# reads ST7789= from /boot.txt at runtime, and DEFAULT_PANEL_ST7789=1 only covers
# a card with no /boot.txt at all. HW_CONFIG 16 sets that default anyway; it is
# passed explicitly below so the build does not depend on that coupling.
#
# USE_PSRAM=1 is what compiles in the PSRAM ROM loader. Note the two uses of that
# flag are unrelated and both predate this script: it also compiles the SA-1
# coprocessor (src/sa1.c), which is dead weight while the core's coproc_stubs.c
# handles the rest. PSRAM presence is detected at runtime via PSRAM_CS_PIN, so
# the same binary still works on a bare RP2350B with no PSRAM fitted.

# NOT "Stop". CMake and the ARM toolchain both write progress to stderr, and
# Windows PowerShell 5.1 turns a native command's stderr into a terminating
# error record when ErrorActionPreference is Stop -- so the first harmless
# banner line aborts the script before anything is built. Exit codes are checked
# explicitly below instead, which is the check that actually matters.
$ErrorActionPreference = "Continue"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_hw16"

Write-Host "Configurando CMake para Waveshare Core2350B (HW_CONFIG 16) + TFT SPI0 + microSD SPI1 + PSRAM..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_core2350b `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=16 `
  -DDEFAULT_PANEL_ST7789=1 `
  -DUSE_PSRAM=1 `
  -DPERIPH_CLOCK_DECOUPLE=1 `
  -DSNES_GSU_RAM_IN_SRAM=ON `
  -DDVIAUDIOFREQ=32000 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

Write-Host "Compilando PicoSNES..." -ForegroundColor Cyan
ninja -C $BuildDir PicoSNES
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] Falha na compilacao. Nenhum UF2 novo foi gerado." -ForegroundColor Red
    exit 1
}

$Uf2Output = "$BuildDir/PicoSNES.uf2"
if (-not (Test-Path $Uf2Output)) {
    Write-Host "[ERRO] Arquivo UF2 nao encontrado apos compilacao." -ForegroundColor Red
    exit 1
}
$AgeMinutes = ((Get-Date) - (Get-Item $Uf2Output).LastWriteTime).TotalMinutes
if ($AgeMinutes -gt 5) {
    Write-Host "[ERRO] UF2 tem $($AgeMinutes.ToString('0')) min -- e o binario de uma build anterior, nao desta." -ForegroundColor Red
    exit 1
}

# Separate destination name from the HW15 script on purpose. The HW16 build
# writes the ST7789/PSRAM firmware, and letting both scripts write "PicoSNES.uf2"
# in the project root means picking up the wrong one for the console you are
# holding -- which reads as "the display changed" rather than "wrong file".
Copy-Item $Uf2Output -Destination ".\PicoSNES_hw16.uf2" -Force
$Size = (Get-Item $Uf2Output).Length
Write-Host "`n[SUCESSO] Gerado com sucesso: PicoSNES_hw16.uf2 ($Size bytes)" -ForegroundColor Green
Write-Host "Copie para /emu/16/PicoSNES.uf2 no cartao MicroSD." -ForegroundColor Yellow
Write-Host "Defina ST7789=1 em /boot.txt para o painel ST7789 do Core2350B." -ForegroundColor Yellow
Write-Host "Com PSRAM presente as ROMs vao para a PSRAM em vez da flash XIP." -ForegroundColor Gray