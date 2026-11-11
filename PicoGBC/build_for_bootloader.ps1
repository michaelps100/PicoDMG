# Build script for PicoGBC targeting pico-bootLoader on Waveshare RP2350 Plus 16MB with ILI9341 display

$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_bootloader"

Write-Host "Configurando CMake para Waveshare RP2350 Plus 16MB + ILI9341 + Bootloader..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_rp2350_plus_16mb `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DDISPLAY_DRIVER=ILI9341 `
  -DRP2040_GB_ST7789_MADCTL=0x28 `
  -DRP2040_GB_DEFAULT_INTERLACE=OFF `
  -DRP2040_GB_SYS_CLOCK_VCO=1440000000 `
  -DRP2040_GB_SYS_CLOCK_DIV1=5 `
  -DBUILD_FOR_BOOTLOADER=ON

Write-Host "Compilando PicoGBC e PicoDMG..." -ForegroundColor Cyan
ninja -C $BuildDir PicoGBC PicoDMG

# Garantir copia dos UF2 para build root e raiz do projeto
Copy-Item "$BuildDir/src/PicoGBC.uf2" "$BuildDir/PicoGBC.uf2" -Force
Copy-Item "$BuildDir/src/PicoGBC.uf2" "PicoGBC.uf2" -Force
Copy-Item "$BuildDir/src/PicoDMG.uf2" "$BuildDir/PicoDMG.uf2" -Force
Copy-Item "$BuildDir/src/PicoDMG.uf2" "PicoDMG.uf2" -Force

$GbcOutput = "$BuildDir/PicoGBC.uf2"
$DmgOutput = "$BuildDir/PicoDMG.uf2"

if (Test-Path $GbcOutput) {
    $Size = (Get-Item $GbcOutput).Length
    Write-Host "`n[SUCESSO] Gerado com sucesso: PicoGBC.uf2 ($Size bytes)" -ForegroundColor Green
    Write-Host "Arquivo disponivel em:" -ForegroundColor Green
    Write-Host "  -> PicoGBC.uf2 (raiz do projeto)" -ForegroundColor Cyan
    Write-Host "  -> $BuildDir/PicoGBC.uf2" -ForegroundColor Cyan
    Write-Host "  -> $BuildDir/src/PicoGBC.uf2" -ForegroundColor Cyan
} else {
    Write-Host "[ERRO] Arquivo PicoGBC.uf2 nao encontrado apos compilacao." -ForegroundColor Red
}

if (Test-Path $DmgOutput) {
    $Size = (Get-Item $DmgOutput).Length
    Write-Host "`n[SUCESSO] Gerado com sucesso: PicoDMG.uf2 ($Size bytes)" -ForegroundColor Green
    Write-Host "Arquivo disponivel em:" -ForegroundColor Green
    Write-Host "  -> PicoDMG.uf2 (raiz do projeto)" -ForegroundColor Cyan
    Write-Host "  -> $BuildDir/PicoDMG.uf2" -ForegroundColor Cyan
    Write-Host "  -> $BuildDir/src/PicoDMG.uf2" -ForegroundColor Cyan
} else {
    Write-Host "[ERRO] Arquivo PicoDMG.uf2 nao encontrado apos compilacao." -ForegroundColor Red
}

Write-Host "`nCopie os arquivos UF2 para o cartao MicroSD em /emu/ ou /emu/15/" -ForegroundColor Yellow

