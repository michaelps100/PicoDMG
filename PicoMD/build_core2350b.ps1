# Build PicoMD for the Waveshare Core2350B console (HW_CONFIG 16).
#
# The board has 16 MB of flash and 8 MB of PSRAM on GP47, so the cartridge ROM is
# read from the SD card straight into PSRAM and the flash cache is skipped --
# a 4 MB Mega Drive cart otherwise spends seconds in erase/program cycles on a
# window the loader image also occupies.
#
# Panel: MADCTL is not a build choice. shared_menu/dmg_panel.c reads ST7789= from
# /boot.txt at runtime, so one firmware drives either module. ILI9341 is 0x28 in
# landscape, the ST7789 is 0x60 (the same value plus MX, measured on the console
# panel, and it keeps BGR=1 so red and blue do not swap). DMG_DEFAULT_PANEL_ST7789
# only covers a card with no /boot.txt at all.

$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_hw16"

Write-Host "Configurando CMake para Waveshare Core2350B (HW_CONFIG 16) + PSRAM GP47..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_core2350b `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=16 `
  -DDEFAULT_PANEL_ST7789=1 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

Write-Host "Compilando PicoMD..." -ForegroundColor Cyan
ninja -C $BuildDir PicoMD

# Check the exit code, not just the file's existence: a failed ninja leaves the
# previous build's .uf2 in place, so a Test-Path alone copies a stale binary and
# reports success. That is how a compile error got shipped as "SUCESSO".
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] ninja falhou (exit $LASTEXITCODE). Nenhum binario gerado." -ForegroundColor Red
    exit 1
}

$Uf2 = "$BuildDir/PicoMD.uf2"
if (Test-Path $Uf2) {
    $Stale = (Get-Item $Uf2).LastWriteTime -lt (Get-Date).AddMinutes(-5)
    if ($Stale) {
        Write-Host "[ERRO] O .uf2 e anterior a este build; nao copiavel." -ForegroundColor Red
        exit 1
    }
    Copy-Item $Uf2 -Destination ".\PicoMD_hw16.uf2" -Force
    $Size = (Get-Item $Uf2).Length
    Write-Host "`n[SUCESSO] PicoMD_hw16.uf2 ($Size bytes)" -ForegroundColor Green
    Write-Host "Copie para o cartao MicroSD em /emu/16/PicoMD.uf2" -ForegroundColor Yellow
    Write-Host "Defina ST7789=1 em /boot.txt para o painel ST7789 do Core2350B." -ForegroundColor Yellow
} else {
    Write-Host "[ERRO] $Uf2 nao encontrado apos compilacao." -ForegroundColor Red
    exit 1
}
