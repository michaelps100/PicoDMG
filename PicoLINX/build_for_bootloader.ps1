# Build script for PicoLINX targeting pico-bootLoader on Waveshare RP2350 Plus 16MB with ILI9341 display + I2S audio

$ErrorActionPreference = "Stop"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_bootloader"

Write-Host "`n[1/3] Configurando CMake para RP2350 + ILI9341 + I2S + Bootloader..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=picolinx `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

Write-Host "`n[2/3] Compilando PicoLINX..." -ForegroundColor Cyan
ninja -C $BuildDir lynx

Write-Host "`n[3/3] Verificando e copiando binarios UF2..." -ForegroundColor Cyan

$PicoUf2 = ".\bin\picolinx\Release\PicoLNX.uf2"
# A failed ninja leaves the previous .uf2 in place, so Test-Path alone can copy a
# stale binary and still print [OK]. Refuse anything older than 5 minutes.
if (Test-Path $PicoUf2) {
    $AgeMin = ((Get-Date) - (Get-Item $PicoUf2).LastWriteTime).TotalMinutes
    if ($AgeMin -gt 5) {
        Write-Host "[ERRO] PicoLNX.uf2 tem $($AgeMin.ToString('F0')) min -- binario obsoleto, recusando copiar." -ForegroundColor Red
        exit 1
    }
    Copy-Item $PicoUf2 -Destination ".\PicoLNX.uf2" -Force
    $PicoSize = (Get-Item $PicoUf2).Length
    Write-Host "[OK] PicoLNX.uf2 gerado com sucesso: $PicoSize bytes" -ForegroundColor Green
    Write-Host "     Destino no SD: /roms/lynx/ (jogos) e /saves/lynx/ (saves/states/cfg)" -ForegroundColor Gray
} else {
    Write-Host "[ERRO] PicoLNX.uf2 nao encontrado!" -ForegroundColor Red
}

Write-Host "`nArquivo pronto na raiz do projeto:" -ForegroundColor Cyan
Write-Host "  -> .\PicoLNX.uf2" -ForegroundColor Green
Write-Host "`nPara carregar via pico-bootLoader, copie para /emu/ no cartao MicroSD." -ForegroundColor Yellow