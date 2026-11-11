# Build PicoSNES for the Waveshare RP2350 Plus 16MB (HW_CONFIG 15) with the
# ILI9341 panel, targeting the pico-bootLoader partition at 0x10080000.
param (
    [switch]$UsePsram = $false
)

# NOT "Stop". CMake and the ARM toolchain write progress to stderr, and Windows
# PowerShell 5.1 promotes a native command's stderr to a terminating error record
# under Stop, aborting on the first banner line. Exit codes are checked below.
$ErrorActionPreference = "Continue"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_bootloader"

$psramVal = "0"
if ($UsePsram) {
    $psramVal = "1"
}

# QUOTED on purpose. PowerShell 5.1 does not expand a variable in a bare native
# argument of the form -DName=$var: it passes the literal text "$var" through to
# the program. CMake then caches USE_PSRAM as the string "$psramVal", which is not
# a recognised false constant, so `if(USE_PSRAM)` evaluates TRUE and PSRAM support
# is silently enabled on a board that has none. That is not hypothetical here --
# it is what had USE_SA1 defined on every build and 32 KB of SA1_Map /
# SA1_WriteMap in .bss (see ENABLE_SA1 in CMakeLists.txt).
$cmakeArgs = @(
  "-S", ".", "-B", $BuildDir, "-G", "Ninja",
  "-DPICO_BOARD=waveshare_rp2350_plus_16mb",
  "-DPICO_PLATFORM=rp2350-arm-s",
  "-DUSE_ILI9341=1",
  "-DHW_CONFIG=15",
  "-DUSE_PSRAM=$psramVal",
  "-DENABLE_SA1=0",
  "-DBUILD_FOR_BOOTLOADER=ON",
  "-DCMAKE_BUILD_TYPE=Release"
)

Write-Host "`n[1/3] Configurando CMake para RP2350 Plus 16MB (PSRAM=$psramVal) + ILI9341 + MAX98357A + Bootloader..." -ForegroundColor Cyan
& cmake @cmakeArgs

Write-Host "`n[2/3] Compilando PicoSNES..." -ForegroundColor Cyan
ninja -C $BuildDir PicoSNES
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] Falha na compilacao. Nenhum UF2 novo foi gerado." -ForegroundColor Red
    exit 1
}

Write-Host "`n[3/3] Verificando e copiando binario UF2..." -ForegroundColor Cyan

# Test-Path alone is not enough: a failed ninja leaves the previous .uf2 sitting
# in the build directory, so the script would copy stale firmware and print OK.
# Require the build to have succeeded AND the artifact to be recent.
$SnesUf2 = "$BuildDir/PicoSNES.uf2"
if (-not (Test-Path $SnesUf2)) {
    Write-Host "[ERRO] PicoSNES.uf2 nao encontrado apos compilacao." -ForegroundColor Red
    exit 1
}
$AgeMinutes = ((Get-Date) - (Get-Item $SnesUf2).LastWriteTime).TotalMinutes
if ($AgeMinutes -gt 5) {
    Write-Host "[ERRO] PicoSNES.uf2 tem $($AgeMinutes.ToString('0')) min -- e o binario de uma build anterior, nao desta." -ForegroundColor Red
    exit 1
}

Copy-Item $SnesUf2 -Destination ".\PicoSNES.uf2" -Force
$SnesSize = (Get-Item $SnesUf2).Length
Write-Host "`n[SUCESSO] PicoSNES.uf2 gerado: $SnesSize bytes" -ForegroundColor Green
Write-Host "  -> .\PicoSNES.uf2        (raiz do projeto)" -ForegroundColor Cyan
Write-Host "  -> $BuildDir/PicoSNES.uf2" -ForegroundColor Cyan
Write-Host "`nCopie para /emu/PicoSNES.uf2 ou /emu/15/PicoSNES.uf2 no cartao MicroSD." -ForegroundColor Yellow
Write-Host "Jogos em 0:/roms/snes/ ; saves, states e config em 0:/saves/snes/." -ForegroundColor Gray