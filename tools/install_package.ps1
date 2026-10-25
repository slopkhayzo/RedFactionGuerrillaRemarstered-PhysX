# Installs a release package (tools\package.py) into the game folder the way a player does: the zip extracted over the
# game folder, after the development deployment (tools\deploy.ps1) is removed, so only the package's files are there.
# Another mod's dinput8.dll (Sledge's) stays: the package's loader goes in as winmm.dll, as the player renames it.
# Back to development builds: tools\deploy.ps1 -ResetIni. Session logs (scripts\rfgphys_runs) stay.
param(
    [string]$Package = '',   # default: the newest dist\rfgphys-*-with-asi-loader.zip
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered'
)

$root = Split-Path -Parent $PSScriptRoot
if (Get-Process -Name rfg -ErrorAction SilentlyContinue) { Write-Output 'rfg.exe is running: close it first'; exit 3 }
if (-not (Test-Path (Join-Path $GameDir 'rfg.exe'))) { Write-Output "rfg.exe not found in $GameDir"; exit 2 }
if (-not $Package) {
    # The development deployment's loader goes below, so the archive with the loader.
    $Package = Get-ChildItem (Join-Path $root 'dist') -Filter 'rfgphys-*-with-asi-loader.zip' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $Package -or -not (Test-Path $Package)) { Write-Output 'no package: make one first (python tools\package.py)'; exit 2 }

$ualHash = 'D5A059AA467A7A7127C8F6169F79FA63FF0F55986EE9EB2FD9A281BEBF2AA2E6'
$dinput8 = Join-Path $GameDir 'dinput8.dll'
$foreignDinput8 = (Test-Path $dinput8) -and (Get-FileHash $dinput8 -Algorithm SHA256).Hash -ne $ualHash

& (Join-Path $PSScriptRoot 'undeploy.ps1') -GameDir $GameDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($foreignDinput8) {
    if (Test-Path (Join-Path $GameDir 'winmm.dll')) {
        Write-Output "other mods' dinput8.dll and winmm.dll are in the game folder; not installing over them"
        exit 4
    }
    $staging = Join-Path ([IO.Path]::GetTempPath()) "rfgphys_install_$PID"
    Expand-Archive -Path $Package -DestinationPath $staging -Force
    if (Test-Path (Join-Path $staging 'dinput8.dll')) { Move-Item (Join-Path $staging 'dinput8.dll') (Join-Path $staging 'winmm.dll') }
    Copy-Item (Join-Path $staging '*') $GameDir -Recurse -Force
    Remove-Item $staging -Recurse -Force
    Write-Output "another mod's dinput8.dll is in the game folder: the package's loader went in as winmm.dll"
} else {
    Expand-Archive -Path $Package -DestinationPath $GameDir -Force
}
$hash = (Get-FileHash $Package -Algorithm SHA256).Hash
# For the test tools only (tools\play_session.ps1 -Release names the package); a player's install has no such file.
"$(Split-Path -Leaf $Package) SHA-256 $hash" | Set-Content (Join-Path $GameDir 'scripts\rfgphys_installed_package.txt') -Encoding ASCII
Write-Output "installed $(Split-Path -Leaf $Package) into $GameDir (SHA-256 $hash)"
exit 0
