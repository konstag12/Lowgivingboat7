# Launch the TEST SCENE (not GTA VI) on Windows 11 x64.
#   powershell -ExecutionPolicy Bypass -File run.ps1                # interactive, 60 FPS
#   powershell -ExecutionPolicy Bypass -File run.ps1 -Fps unlimited
#   powershell -ExecutionPolicy Bypass -File run.ps1 -Bench         # 3 x 60 s per mode, writes results\
param([string]$Fps = '60', [switch]$Bench)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$exe = @('build\msvc\Release\pcport_testscene.exe', 'build\win64\pcport_testscene.exe') |
       Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { throw 'pcport_testscene.exe not found - run build.ps1 (or build.sh on Linux) first' }
New-Item -ItemType Directory -Force results | Out-Null
if (-not $Bench) { & $exe --fps $Fps; exit $LASTEXITCODE }
$sum = 'results\bench_windows.txt'
foreach ($m in '30', '60', 'unlimited') {
  foreach ($r in 1..3) {
    & $exe --bench 60 --fps $m --width 1920 --height 1080 --csv "results\frames_${m}_run$r.csv" --summary $sum
  }
}
Get-Content $sum
