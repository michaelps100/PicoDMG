# Build PicoGBC and PicoDMG for the Waveshare Core2350B console (HW_CONFIG 16).
#
# The board has 16 MB of flash and 8 MB of PSRAM on GP47, so the cartridge ROM
# is read from the SD card straight into PSRAM and the flash cache is skipped
# entirely (CMake forces RP2040_GB_USE_PSRAM=1 for this config).
#
# Panel: MADCTL is the one value that differs between the two panels the console
# can carry. ILI9341 is 0x28 in landscape; the ST7789 on the Core2350B needs
# 0x60, which is 0x28+MX -- that bit is measured, not derived from a datasheet,
# because the mirror axis depends on how the controller is bonded to the glass.
# Both keep BGR=1 so red and blue do not swap. Set -DRP2040_GB_ST7789_MADCTL=0x28
# below if your console has the ILI9341 module instead.

$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_hw16"

Write-Host "Configurando CMake para Waveshare Core2350B (HW_CONFIG 16) + PSRAM GP47 + TFT SPI0..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_core2350b `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DDISPLAY_DRIVER=ST7789 `
  -DHW_CONFIG=16 `
  -DRP2040_GB_ST7789_MADCTL=0x60 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

Write-Host "Compilando PicoGBC e PicoDMG..." -ForegroundColor Cyan
ninja -C $BuildDir PicoGBC PicoDMG

$Ok = $true
foreach ($Name in @("PicoGBC", "PicoDMG")) {
    $Uf2 = "$BuildDir/src/$Name.uf2"
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
Write-Host "  PicoGBC_hw16.uf2 -> /emu/16/PicoGBC.uf2   (lê 0:/roms/gbc)" -ForegroundColor Yellow
Write-Host "  PicoDMG_hw16.uf2 -> /emu/16/PicoDMG.uf2   (lê 0:/roms/gb)" -ForegroundColor Yellow
Write-Host "Defina ST7789=1 em /boot.txt para o painel ST7789 do Core2350B." -ForegroundColor Yellow
