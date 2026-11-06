# Builds tools/host_runner/a26run.exe with MSVC (no WSL). Host evidence tool only.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vcvars = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vcvars) { Write-Host '[ERRO] vcvars64.bat nao encontrado'; exit 1 }
$out = Join-Path $root 'build_host_win'
New-Item -ItemType Directory -Force $out | Out-Null
$src = @(
  'tools\host_runner\a26run.c',
  'core2600\a26_core.c', 'core2600\cpu.c', 'core2600\video.c', 'core2600\cartridge.c',
  'core2600\memory.c', 'core2600\timer.c', 'core2600\delayed_writer.c', 'core2600\input.c',
  'atari\atari_audio.c', 'third_party\mcume_picovcs\Tiasound.c'
) | ForEach-Object { '"' + (Join-Path $root $_) + '"' }
$flags = "/nologo /O2 /std:c11 /J /W1 /D_CRT_SECURE_NO_WARNINGS /FI`"$root\tools\host_runner\host_compat.h`" " +
         "/I`"$root`" /I`"$root\core2600`" /I`"$root\third_party\mcume_picovcs`" /Fo`"$out\\`" /Fe`"$out\a26run.exe`""
$cmd = "`"$($vcvars.FullName)`" >nul && cl $flags $($src -join ' ')"
$ErrorActionPreference = 'Continue'
cmd /c $cmd 2>&1 | Where-Object { $_ -notmatch 'vswhere' }
if ($LASTEXITCODE -ne 0) { Write-Host "[ERRO] cl falhou (exit $LASTEXITCODE)"; exit $LASTEXITCODE }
Write-Host "[OK] $out\a26run.exe"
