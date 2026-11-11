# Build PicoPCE for the Waveshare Core2350B console (HW_CONFIG 16) with PSRAM
# disk/CD-ROM support (.chd/.cue) via ENABLE_CHD.
#
# The panel (ILI9341 or ST7789) is not chosen here: shared_menu/dmg_panel.c reads
# ST7789= from /boot.txt at runtime, and DEFAULT_PANEL_ST7789=1 only covers the
# case where the card has no /boot.txt. HW_CONFIG 16 sets that default anyway;
# it is passed explicitly below so the build does not depend on that coupling.

param([switch]$RondoFix)

$ErrorActionPreference = "Stop"

$env:PATH = "C:\Users\Michael\.pico-sdk\picotool\2.3.1\picotool;C:\Users\Michael\.pico-sdk\tools\2.3.1\pioasm;C:\Users\Michael\.pico-sdk\toolchain\15_2_Rel1\bin;C:\Users\Michael\.pico-sdk\cmake\v3.28.6\bin;C:\Users\Michael\.pico-sdk\ninja\v1.12.1;$env:PATH"
$env:PICO_SDK_PATH = "C:\Users\Michael\.pico-sdk\sdk\2.3.1"

$BuildDir = "build_hw16"
$DiagnosticsEnabled = "ON"
$OutputStem = "PicoPCE_hw16"
if ($env:PCE_DIAGNOSTICS -eq "1" -or $RondoFix) {
  $BuildDir = "build_hw16_diag"
  $OutputStem = "PicoPCE_hw16_diag_r3"
}
if ($RondoFix) {
  $OutputStem = "PicoPCE_hw16_rondo_fix"
}

Write-Host "Configurando CMake para Waveshare Core2350B (HW_CONFIG 16) + TFT SPI0 + microSD SPI1 + CHD..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G Ninja `
  -DPICO_BOARD=waveshare_core2350b `
  -DPICO_PLATFORM=rp2350-arm-s `
  -DUSE_ILI9341=1 `
  -DHW_CONFIG=16 `
  -DDEFAULT_PANEL_ST7789=1 `
  -DENABLE_CHD=1 `
  -DPCE_DIAGNOSTICS:BOOL=$DiagnosticsEnabled `
  -DCDLOAD_DIAG_DRAW=1 `
  -DBUILD_FOR_BOOTLOADER=ON `
  -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] CMake falhou. Nao sera copiado um UF2 anterior." -ForegroundColor Red
    exit 1
}

Write-Host "Compilando PicoPCE..." -ForegroundColor Cyan
ninja -C $BuildDir PicoPCE
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] Falha na compilacao. Nenhum UF2 foi gerado." -ForegroundColor Red
    exit 1
}

$Uf2Output = "$BuildDir/PicoPCE.uf2"
if (Test-Path $Uf2Output) {
    Copy-Item $Uf2Output -Destination ".\$OutputStem.uf2" -Force
    Copy-Item "$BuildDir/PicoPCE.elf" -Destination ".\$OutputStem.elf" -Force
    Copy-Item "$BuildDir/PicoPCE.elf.map" -Destination ".\$OutputStem.elf.map" -Force
    Get-FileHash ".\$OutputStem.uf2", ".\$OutputStem.elf" -Algorithm SHA256 |
      Select-Object @{Name="File";Expression={Split-Path $_.Path -Leaf}}, Hash |
      ConvertTo-Json | Set-Content ".\$OutputStem.hashes.json" -Encoding UTF8
    $Size = (Get-Item $Uf2Output).Length
    $OutputName = "$OutputStem.uf2"
    Write-Host "`n[SUCESSO] Gerado com sucesso: $OutputName ($Size bytes)" -ForegroundColor Green
    Write-Host "Relatorio automatico de falha ativo; ELF/map e hashes salvos com o mesmo nome." -ForegroundColor Yellow
    Write-Host "Copie o UF2 para o cartao MicroSD em /emu/16/PicoPCE.uf2" -ForegroundColor Yellow
    Write-Host "Defina ST7789=1 em /boot.txt para o painel ST7789 do Core2350B." -ForegroundColor Yellow
} else {
    Write-Host "[ERRO] Arquivo UF2 nao encontrado apos compilacao." -ForegroundColor Red
    exit 1
}
