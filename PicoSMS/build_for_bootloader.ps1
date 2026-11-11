# Build script for PicoSMS & PicoGG targeting pico-bootLoader on Waveshare RP2350 Plus 16MB with ILI9341 display

$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_bootloader"

Write-Host "`n[1/3] Configurando CMake para RP2350 + ILI9341 + MAX98357A + Bootloader..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_rp2350_plus_16mb `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=15 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

Write-Host "`n[2/3] Compilando PicoSMS e PicoGG..." -ForegroundColor Cyan
ninja -C $BuildDir PicoSMS PicoGG

Write-Host "`n[3/3] Verificando e copiando binarios UF2..." -ForegroundColor Cyan

$SmsUf2 = "$BuildDir/PicoSMS.uf2"
if (Test-Path $SmsUf2) {
    Copy-Item $SmsUf2 -Destination ".\PicoSMS.uf2" -Force
    $SmsSize = (Get-Item $SmsUf2).Length
    Write-Host "[OK] PicoSMS.uf2 gerado com sucesso: $SmsSize bytes" -ForegroundColor Green
    Write-Host "     Destino no SD: /roms/ms/ (jogos) e /saves/ms/ (saves/states)" -ForegroundColor Gray
} else {
    Write-Host "[ERRO] PicoSMS.uf2 nao encontrado!" -ForegroundColor Red
}

$GgUf2 = "$BuildDir/PicoGG.uf2"
if (Test-Path $GgUf2) {
    Copy-Item $GgUf2 -Destination ".\PicoGG.uf2" -Force
    $GgSize = (Get-Item $GgUf2).Length
    Write-Host "[OK] PicoGG.uf2 gerado com sucesso: $GgSize bytes" -ForegroundColor Green
    Write-Host "     Destino no SD: /roms/gg/ (jogos) e /saves/gg/ (saves/states)" -ForegroundColor Gray
} else {
    Write-Host "[ERRO] PicoGG.uf2 nao encontrado!" -ForegroundColor Red
}

Write-Host "`nArquivos prontos na raiz do projeto:" -ForegroundColor Cyan
Write-Host "  -> .\PicoSMS.uf2" -ForegroundColor Green
Write-Host "  -> .\PicoGG.uf2" -ForegroundColor Green
Write-Host "`nPara carregar via pico-bootLoader, copie para /emu/ no cartao MicroSD." -ForegroundColor Yellow

