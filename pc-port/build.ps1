# Reproducible build on Windows 11 x64 (Visual Studio 2022 Build Tools or MSVC + CMake).
#   powershell -ExecutionPolicy Bypass -File build.ps1
# Log: results\build_log_windows.txt
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force results | Out-Null
$log = 'results\build_log_windows.txt'
"=== pc-port build $(Get-Date -Format o) ===" | Tee-Object $log
cmake -S . -B build\msvc -G "Visual Studio 17 2022" -A x64 2>&1 | Tee-Object $log -Append
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build build\msvc --config Release 2>&1 | Tee-Object $log -Append
if ($LASTEXITCODE) { throw "build failed" }
"=== build OK: build\msvc\Release\pcport_testscene.exe ===" | Tee-Object $log -Append
