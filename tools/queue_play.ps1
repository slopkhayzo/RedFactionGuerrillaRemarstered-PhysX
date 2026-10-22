# Puts an interactive play session into the shared test queue, so it never overlaps other agents' tests, and tells
# you when the game is about to start. The game launches by itself when your turn comes; quit it normally when done.
#
#   powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name watchtower -Record
#   powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name watchtower-debris -DebrisCollide
#
# Closing this window does not cancel the session; cancel it with:
#   & 'D:\Githubs\_agents\queue_executor\queue.ps1' cancel -Id <id>
param(
    [string]$Name = 'play',
    [int]$Minutes = 45,
    [Alias('WorldOnlyDebrisCollide')]
    [switch]$DebrisCollide,
    [int]$DebrisCollideMode = 1,   # 1 debris collides with debris, 2 it also damages and breaks up debris
    [switch]$Record,
    [switch]$BlockSaveWrites,
    [switch]$ReleaseSettings,      # the release package's behaviour settings on the deployed build (works with -Record)
    [switch]$Release,              # the installed release package with its own settings (tools\install_package.ps1)
    [string]$GameDir = '',         # another copy of the game, e.g. the Steam one (default: the GOG copy)
    [string[]]$Set = @()
)

$queue = 'D:\Githubs\_agents\queue_executor\queue.ps1'
$root = Split-Path -Parent $PSScriptRoot

$jobArgs = @('-Name', $Name, '-Minutes', "$Minutes")
if ($DebrisCollide) { $jobArgs += @('-DebrisCollide', '-DebrisCollideMode', "$DebrisCollideMode") }
if ($Record) { $jobArgs += '-Record' }
if ($BlockSaveWrites) { $jobArgs += '-BlockSaveWrites' }
if ($ReleaseSettings) { $jobArgs += '-ReleaseSettings' }
if ($Release) { $jobArgs += '-Release' }
if ($GameDir) { $jobArgs += @('-GameDir', $GameDir) }
if ($Set.Count) { $jobArgs += @('-Set', ($Set -join ';')) }  # a parameter can only be passed once

$label = "RFG play session (user at the keyboard): $Name"
if ($Record) { $label += ' [recording]' }
if ($DebrisCollide) { $label += " [DebrisCollide=$DebrisCollideMode]" }
if ($ReleaseSettings) { $label += ' [release settings]' }
if ($Release) { $label += ' [release package]' }

$out = & $queue submit -Name $label -Owner 'RedFactionGuerrillaRemarstered-PhysX' -WorkingDirectory $root `
    -TimeoutMinutes ($Minutes + 5) -File (Join-Path $PSScriptRoot 'play_session.ps1') -Arguments $jobArgs
$idLine = $out | Where-Object { $_ -match '^id=' } | Select-Object -First 1
if (-not $idLine) { $out | Write-Host; Write-Host 'submit failed'; exit 1 }
$id = $idLine.Substring(3)
Write-Host "queued as $id"

$lastPosition = ''
while ($true) {
    $status = & $queue status -Id $id
    $state = (($status | Where-Object { $_ -match '^state=' }) -replace '^state=', '')
    if ($state -ne 'pending') { break }
    $position = (($status | Where-Object { $_ -match '^position=' }) -replace '^position=', '')
    if ($position -ne $lastPosition) {
        Write-Host "$(Get-Date -Format HH:mm:ss) waiting for other tests to finish: $position"
        $lastPosition = $position
    }
    Start-Sleep -Seconds 5
}

if ($state -eq 'running') {
    [console]::Beep(880, 250); [console]::Beep(1175, 250)
    Write-Host "$(Get-Date -Format HH:mm:ss) your turn: the game is starting now. Play, then quit the game normally."
}
& $queue wait -Id $id -Tail 40
