# Build the pico-bootLoader for HW_CONFIG 15 (Waveshare RP2350 Plus 16MB + ILI9341,
# the PicoDMG console).
#
# Configures explicitly, like build_core2350b.ps1. The old version only ran ninja
# in build_ili9341/, whose cache had drifted to HW_CONFIG 2 / USE_ILI9341=0 (DVI),
# so it silently produced a DVI loader instead of the console one. build_hw15/ is
# a fresh directory so no stale cache can do that again.
#
# -Os is inherited from the top-level CMakeLists on purpose: removing it
# deadlocks the Core 1 DMA chain (see the note in main.cpp above initAll).

$ErrorActionPreference = "Continue"

$env:PATH = "$env:USERPROFILE\.pico-sdk\picotool\2.3.1\picotool;$env:USERPROFILE\.pico-sdk\tools\2.3.1\pioasm;$env:USERPROFILE\.pico-sdk\toolchain\15_2_Rel1\bin;$env:USERPROFILE\.pico-sdk\cmake\v3.28.6\bin;$env:USERPROFILE\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\sdk\2.3.1"

$buildDir = "$PSScriptRoot\build_hw15"

cmake -S "$PSScriptRoot" -B $buildDir -G Ninja `
    -DUSE_ILI9341=1 `
    -DHW_CONFIG=15 `
    -DPICO_BOARD=waveshare_rp2350_plus_16mb `
    -DDEFAULT_PANEL_ST7789=0
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

ninja -C $buildDir -j 8
if ($LASTEXITCODE -ne 0) { throw "ninja failed ($LASTEXITCODE)" }

Write-Host ""
Write-Host "OK: $buildDir\pico-bootLoader.uf2"
