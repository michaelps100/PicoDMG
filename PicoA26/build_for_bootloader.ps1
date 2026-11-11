# Build script for PicoA26 targeting pico-bootLoader on Waveshare RP2350 Plus 16MB with ILI9341 display

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

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] CMake falhou; build cancelado." -ForegroundColor Red
    exit 1
}

Write-Host "Compilando PicoA26..." -ForegroundColor Cyan
ninja -C $BuildDir PicoA26

# Check the exit code, not just whether the file exists: a failed ninja leaves
# the previous build's .uf2 in place, so Test-Path alone copies a stale binary
# and reports success.
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] ninja falhou (exit $LASTEXITCODE). Nenhum binario gerado." -ForegroundColor Red
    exit 1
}

$Uf2Output = "$BuildDir/PicoA26.uf2"
if (Test-Path $Uf2Output) {
    # A successful no-op Ninja build is valid; file age alone does not imply
    # stale firmware. CMake and Ninja exit codes were checked above.
    Copy-Item $Uf2Output -Destination ".\PicoA26.uf2" -Force
    $Size = (Get-Item $Uf2Output).Length
    Write-Host "`n[SUCESSO] Gerado com sucesso: PicoA26.uf2 ($Size bytes)" -ForegroundColor Green
    Write-Host "Arquivo disponivel em:" -ForegroundColor Green
    Write-Host "  -> PicoA26.uf2 (raiz do projeto)" -ForegroundColor Cyan
    Write-Host "  -> $BuildDir/PicoA26.uf2" -ForegroundColor Cyan
    Write-Host "`nCopie PicoA26.uf2 para o cartao MicroSD em /emu/PicoA26.uf2 ou /emu/15/PicoA26.uf2" -ForegroundColor Yellow
} else {
    Write-Host "[ERRO] Arquivo UF2 nao encontrado apos compilacao." -ForegroundColor Red
}

