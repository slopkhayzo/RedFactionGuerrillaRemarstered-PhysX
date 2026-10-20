# Configures and builds the plugin (Win32) with the Visual Studio 2026 toolchain.
# Run it through the shared queue: queue.ps1 submit -Name '...' -Owner RedFactionGuerrillaRemarstered-PhysX -File tools\build.ps1
param([string]$Config = 'RelWithDebInfo')

$root = Split-Path -Parent $PSScriptRoot
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

& $cmake -S $root -B (Join-Path $root 'build') -G 'Visual Studio 18 2026' -A Win32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build (Join-Path $root 'build') --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$asi = Join-Path $root "build\$Config\rfgphys.asi"
Get-Item $asi | Select-Object FullName, Length, LastWriteTime | Format-List | Out-String | Write-Output
exit 0
