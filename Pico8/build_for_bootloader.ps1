# Build Pico8 for Waveshare RP2350 Plus 16MB (HW_CONFIG 15, RP2350A + 8 MB PSRAM on GP0),
# targeting the pico-bootLoader partition at 0x10080000.

$ErrorActionPreference = "Continue"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_hw15"

Write-Host "Configurando CMake para Waveshare RP2350 Plus (HW_CONFIG 15)..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DY8_ARCH=pico `
  -DY8_PLATFORM=picodmg `
  -DHW_CONFIG=15 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DY8_EXPERIMENTAL_GENGC=ON `
  -DY8_LUA_REPL=ON `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DPICO_BOARD=waveshare_rp2350_plus_16mb `
  -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] Falha no CMake configure ($LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host "Compilando Pico8 (HW15)..." -ForegroundColor Cyan
ninja -C $BuildDir Pico8
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] Falha na compilacao. Nenhum UF2 novo foi gerado." -ForegroundColor Red
    exit 1
}

$Uf2Output = "$BuildDir/Pico8.uf2"
if (-not (Test-Path $Uf2Output)) {
    Write-Host "[ERRO] Arquivo UF2 nao encontrado apos compilacao." -ForegroundColor Red
    exit 1
}

Copy-Item $Uf2Output -Destination ".\Pico8_hw15.uf2" -Force
Copy-Item $Uf2Output -Destination ".\Pico8.uf2" -Force
$Size = (Get-Item $Uf2Output).Length
Write-Host "`n[SUCESSO] Gerado com sucesso: Pico8_hw15.uf2 ($Size bytes)" -ForegroundColor Green
Write-Host "Copie para /emu/15/Pico8.uf2 no cartao MicroSD." -ForegroundColor Yellow

