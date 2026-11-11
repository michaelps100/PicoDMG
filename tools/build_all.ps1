# Compila o bootloader e todos os emuladores.
#
#   .\tools\build_all.ps1 -Board HW16     # Core2350B / ST7789
#   .\tools\build_all.ps1 -Board HW15     # RP2350 Plus / ILI9341
#   .\tools\build_all.ps1 -Board All
#   .\tools\build_all.ps1 -Board HW16 -Only PicoNES,PicoSMS

param(
    [ValidateSet("HW15", "HW16", "All")] [string] $Board = "All",
    [string[]] $Only
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

$targets = @(
    "pico-bootLoader", "Pico8", "PicoA26", "PicoGBC", "PicoLINX", "PicoMD",
    "PicoNES", "PicoNGP", "PicoPCE", "PicoSMS", "PicoSNES", "PicoWS"
)
# Via "powershell -File", "-Only A,B" chega como um unico texto "A,B".
if ($Only) {
    $Only = $Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    $unknown = $Only | Where-Object { $targets -notcontains $_ }
    if ($unknown) { Write-Host "Alvo desconhecido: $($unknown -join ', ')" -ForegroundColor Red; exit 1 }
    $targets = $targets | Where-Object { $Only -contains $_ }
}

$boards = if ($Board -eq "All") { @("HW15", "HW16") } else { @($Board) }
$results = [ordered]@{}

foreach ($b in $boards) {
    foreach ($d in $targets) {
        $script = if ($b -eq "HW16") { "build_core2350b.ps1" }
                  elseif ($d -eq "pico-bootLoader") { "build_ili9341.ps1" }
                  else { "build_for_bootloader.ps1" }
        $key = "$d [$b]"
        $path = Join-Path $root $d
        if (-not (Test-Path (Join-Path $path $script))) { $results[$key] = "sem $script"; continue }

        Write-Host "`n>>> $key ($script)" -ForegroundColor Cyan
        Push-Location $path
        try {
            & powershell -NoProfile -ExecutionPolicy Bypass -File ".\$script"
            $results[$key] = if ($LASTEXITCODE -eq 0) { "OK" } else { "FALHOU ($LASTEXITCODE)" }
        } catch {
            $results[$key] = "ERRO: $_"
        } finally {
            Pop-Location
        }
    }
}

Write-Host "`n==== RESUMO ====" -ForegroundColor Cyan
$failed = 0
foreach ($k in $results.Keys) {
    $s = $results[$k]
    if ($s -eq "OK") { Write-Host "  $k : OK" -ForegroundColor Green }
    else { Write-Host "  $k : $s" -ForegroundColor Red; $failed++ }
}
exit $failed
