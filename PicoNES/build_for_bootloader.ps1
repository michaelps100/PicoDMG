# Build script for PicoNES targeting pico-bootLoader on Waveshare RP2350 Plus 16MB with ILI9341 display

$ErrorActionPreference = "Stop"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_bootloader"

Write-Host "Configurando CMake para Waveshare RP2350 Plus 16MB + ILI9341 + MAX98357A + Bootloader..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_rp2350_plus_16mb `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=15 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

Write-Host "Compilando PicoNES..." -ForegroundColor Cyan
ninja -C $BuildDir PicoNES

$Uf2Output = "$BuildDir/PicoNES.uf2"
if (Test-Path $Uf2Output) {
    Copy-Item $Uf2Output -Destination ".\PicoNES.uf2" -Force
    $Size = (Get-Item $Uf2Output).Length
    Write-Host "`n[SUCESSO] Gerado com sucesso: PicoNES.uf2 ($Size bytes)" -ForegroundColor Green
    Write-Host "Arquivo disponivel em:" -ForegroundColor Green
    Write-Host "  -> PicoNES.uf2 (raiz do projeto)" -ForegroundColor Cyan
    Write-Host "  -> $BuildDir/PicoNES.uf2" -ForegroundColor Cyan
    Write-Host "`nCopie PicoNES.uf2 para o cartao MicroSD em /emu/PicoNES.uf2 ou /emu/15/PicoNES.uf2" -ForegroundColor Yellow
} else {
    Write-Host "[ERRO] Arquivo UF2 nao encontrado apos compilacao." -ForegroundColor Red
}
