# Removes everything tools\deploy.ps1 added to the game folder.
param(
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered',
    [switch]$KeepIni
)

$ualHash = 'D5A059AA467A7A7127C8F6169F79FA63FF0F55986EE9EB2FD9A281BEBF2AA2E6'
if (Get-Process -Name rfg -ErrorAction SilentlyContinue) { Write-Output 'rfg.exe is running: close it first'; exit 3 }

$loader = Join-Path $GameDir 'dinput8.dll'
if (Test-Path $loader) {
    if ((Get-FileHash $loader -Algorithm SHA256).Hash -eq $ualHash) { Remove-Item $loader }
    else { Write-Output 'dinput8.dll is not the loader we deployed; left in place' }
}
foreach ($f in 'dinput8.ini', 'dinput8.log') {
    $p = Join-Path $GameDir $f
    if (Test-Path $p) { Remove-Item $p }
}

$scripts = Join-Path $GameDir 'scripts'
foreach ($f in 'rfgphys.asi', 'rfgphys.pdb', 'rfgphys_installed_package.txt', 'rfgphysx\rfgphysx_host.exe',
        'rfgphysx\rfgphysx_host.pdb', 'rfgphysx\PhysXGpu_64.dll') {
    $p = Join-Path $scripts $f
    if (Test-Path $p) { Remove-Item $p }
}
if (-not $KeepIni -and (Test-Path (Join-Path $scripts 'rfgphys.ini'))) { Remove-Item (Join-Path $scripts 'rfgphys.ini') }
# What a release package (tools\package.py) adds besides.
foreach ($p in (Join-Path $GameDir 'rfgphys_README.txt'), (Join-Path $GameDir 'CrashDumps\README.txt')) {
    if (Test-Path $p) { Remove-Item $p }
}
if (Test-Path (Join-Path $GameDir 'rfgphys_licenses')) { Remove-Item -Recurse (Join-Path $GameDir 'rfgphys_licenses') }
foreach ($d in (Join-Path $scripts 'rfgphysx'), $scripts, (Join-Path $GameDir 'CrashDumps')) {
    if ((Test-Path $d) -and -not (Get-ChildItem $d -Force)) { Remove-Item $d }
}
Write-Output "removed rfgphys and Ultimate ASI Loader from $GameDir"
exit 0
