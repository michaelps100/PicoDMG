# Build script for PicoPCE targeting pico-bootLoader on Waveshare RP2350 Plus 16MB with ILI9341 display

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
  -DENABLE_CHD=0 `
  -DPCE_DIAGNOSTICS:BOOL=ON `
  -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] CMake falhou. Nenhum firmware anterior sera copiado." -ForegroundColor Red
    exit 1
}

Write-Host "Compilando PicoPCE..." -ForegroundColor Cyan
ninja -C $BuildDir PicoPCE
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERRO] Falha na compilacao. Nenhum firmware anterior sera copiado." -ForegroundColor Red
    exit 1
}

$Uf2Output = "$BuildDir/PicoPCE.uf2"
if (Test-Path $Uf2Output) {
    Copy-Item $Uf2Output -Destination ".\PicoPCE.uf2" -Force
    Copy-Item "$BuildDir/PicoPCE.elf" -Destination ".\PicoPCE.elf" -Force
    Copy-Item "$BuildDir/PicoPCE.elf.map" -Destination ".\PicoPCE.elf.map" -Force
    Get-FileHash ".\PicoPCE.uf2", ".\PicoPCE.elf" -Algorithm SHA256 |
      Select-Object @{Name="File";Expression={Split-Path $_.Path -Leaf}}, Hash |
      ConvertTo-Json | Set-Content ".\PicoPCE.hashes.json" -Encoding UTF8
    $Size = (Get-Item $Uf2Output).Length
    Write-Host "`n[SUCESSO] Gerado com sucesso: PicoPCE.uf2 ($Size bytes)" -ForegroundColor Green
    Write-Host "Arquivo disponivel em:" -ForegroundColor Green
    Write-Host "  -> PicoPCE.uf2 (raiz do projeto)" -ForegroundColor Cyan
    Write-Host "  -> $BuildDir/PicoPCE.uf2" -ForegroundColor Cyan
    Write-Host "Relatorio automatico de falha ativo; ELF/map e hashes preservados." -ForegroundColor Yellow
    Write-Host "`nCopie PicoPCE.uf2 para o cartao MicroSD em /emu/PicoPCE.uf2 ou /emu/15/PicoPCE.uf2" -ForegroundColor Yellow
} else {
    Write-Host "[ERRO] Arquivo UF2 nao encontrado apos compilacao." -ForegroundColor Red
    exit 1
}

