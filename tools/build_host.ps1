# Builds the 64-bit side of the PhysX experiment (host\: rfgphysx_bench.exe, later the PhysX host) into build-host\.
# Needs PhysX built first (tools\build_physx.cmd). Run it through the shared queue:
#   queue.ps1 submit -Name '...' -Owner RedFactionGuerrillaRemarstered-PhysX -File tools\build_host.ps1
param([string]$Config = 'Release')

$root = Split-Path -Parent $PSScriptRoot
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$out = Join-Path $root 'build-host'

& $cmake -S (Join-Path $root 'host') -B $out -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build $out --config $Config --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Get-ChildItem (Join-Path $out $Config) -Include *.exe, *.dll -Recurse | Select-Object Name, Length, LastWriteTime |
    Format-Table -AutoSize | Out-String | Write-Output
exit 0
