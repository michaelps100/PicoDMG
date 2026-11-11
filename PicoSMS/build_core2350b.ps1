# Build PicoSMS and PicoGG for the Waveshare Core2350B console (HW_CONFIG 16).
#
# The board has 16 MB of flash and 8 MB of PSRAM on GP47, so the cartridge ROM is
# read from the SD card straight into PSRAM and the flash cache is skipped.
#
# Panel: MADCTL is not a build choice. shared_menu/dmg_panel.c reads ST7789= from
# /boot.txt at runtime, so one firmware drives either module. ILI9341 is 0x28 in
# landscape, the ST7789 is 0x60 (the same value plus MX, measured on the console
# panel, and it keeps BGR=1 so red and blue do not swap). DMG_DEFAULT_PANEL_ST7789
# only covers a card with no /boot.txt at all.

$ErrorActionPreference = "Stop"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

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

Write-Host "Compilando PicoSMS e PicoGG..." -ForegroundColor Cyan
ninja -C $BuildDir PicoSMS PicoGG
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] Falha na compilacao com ninja." -ForegroundColor Red
    exit $LASTEXITCODE
}

$Ok = $true
foreach ($Name in @("PicoSMS", "PicoGG")) {
    $Uf2 = "$BuildDir/$Name.uf2"
    if (Test-Path $Uf2) {
        Copy-Item $Uf2 -Destination ".\${Name}_hw16.uf2" -Force
        $Size = (Get-Item $Uf2).Length
        Write-Host "`n[SUCESSO] ${Name}_hw16.uf2 ($Size bytes)" -ForegroundColor Green
    } else {
        Write-Host "[ERRO] $Uf2 nao encontrado apos compilacao." -ForegroundColor Red
        $Ok = $false
    }
}

if (-not $Ok) { exit 1 }

Write-Host "`nCopie para o cartao MicroSD:" -ForegroundColor Yellow
Write-Host "  PicoSMS_hw16.uf2 -> /emu/16/PicoSMS.uf2   (le 0:/roms/ms)" -ForegroundColor Yellow
Write-Host "  PicoGG_hw16.uf2  -> /emu/16/PicoGG.uf2    (le 0:/roms/gg)" -ForegroundColor Yellow
Write-Host "Defina ST7789=1 em /boot.txt para o painel ST7789 do Core2350B." -ForegroundColor Yellow
