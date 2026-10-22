# Build script for PicoWS targeting pico-bootLoader on Waveshare RP2350 Plus 16MB with ILI9341 display

$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_bootloader"

Write-Host "`n[1/3] Configurando CMake para RP2350 + ILI9341 + Bootloader..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_rp2350_plus_16mb `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=15 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release

Write-Host "`n[2/3] Compilando PicoWS..." -ForegroundColor Cyan
ninja -C $BuildDir PicoWS

# Check the exit code, not just whether the file exists: a failed ninja leaves
# the previous build's .uf2 in place, so Test-Path alone copies a stale binary
# and reports success.
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] ninja falhou (exit $LASTEXITCODE). Nenhum binario gerado." -ForegroundColor Red
    exit 1
}

Write-Host "`n[3/3] Verificando e copiando binarios UF2..." -ForegroundColor Cyan

$WsUf2 = "$BuildDir/PicoWS.uf2"
if (Test-Path $WsUf2) {
    if ((Get-Item $WsUf2).LastWriteTime -lt (Get-Date).AddMinutes(-5)) {
        Write-Host "[ERRO] O .uf2 e anterior a este build; nao copiavel." -ForegroundColor Red
        exit 1
    }
    Copy-Item $WsUf2 -Destination ".\PicoWS.uf2" -Force
    $WsSize = (Get-Item $WsUf2).Length
    Write-Host "[OK] PicoWS.uf2 gerado com sucesso: $WsSize bytes" -ForegroundColor Green
    Write-Host "     Destino no SD: /roms/ws/ (jogos) e /saves/ws/ (saves/states/cfg)" -ForegroundColor Gray
} else {
    Write-Host "[ERRO] PicoWS.uf2 nao encontrado!" -ForegroundColor Red
    exit 1
}

Write-Host "`nArquivo pronto na raiz do projeto:" -ForegroundColor Cyan
Write-Host "  -> .\PicoWS.uf2" -ForegroundColor Green
Write-Host "`nPara carregar via pico-bootLoader, copie para /emu/ no cartao MicroSD." -ForegroundColor Yellow

