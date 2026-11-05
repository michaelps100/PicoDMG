# Build the pico-bootLoader for HW_CONFIG 16 (Waveshare Core2350B).
#
# Differences from build_ili9341.ps1, all of which come from HW_CONFIG 16 in
# pico_shared/BoardConfigs.cmake:
#   - PICO_BOARD is not force-set, so the block's own
#     PICO_BOARD=waveshare_core2350b stands.
#   - PSRAM_CS_PIN=47 instead of 0, and the board header brings its own 16 MB
#     flash / IOVDD / SMPS setup.
#   - DEFAULT_PANEL_ST7789=1, so a console with no /boot.txt still comes up on
#     the panel the board actually has. ST7789= in /boot.txt still wins.
#
# -Os is inherited from the top-level CMakeLists on purpose: removing it
# deadlocks the Core 1 DMA chain (see the note in main.cpp above initAll).

# Native tools (cmake/ninja) write progress to stderr, which PowerShell would
# otherwise turn into a terminating error under "Stop". Use "Continue" and gate
# on $LASTEXITCODE instead.
$ErrorActionPreference = "Continue"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$buildDir = "C:\dev\PicoDMG-Temp\pico-bootLoader\build_hw16"

cmake -S "C:\dev\PicoDMG-Temp\pico-bootLoader" -B $buildDir -G Ninja `
    -DUSE_ILI9341=1 `
    -DHW_CONFIG=16 `
    -DDEFAULT_PANEL_ST7789=1
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

ninja -C $buildDir -j 8
if ($LASTEXITCODE -ne 0) { throw "ninja failed ($LASTEXITCODE)" }

Write-Host ""
Write-Host "OK: $buildDir\pico-bootLoader.uf2"
