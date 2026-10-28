# Queue job: launches rfg.exe with the deployed plugin for an unattended run, waits for it to end, and collects the
# run folder. Save writes are always blocked and the real save folder is verified untouched afterwards.
# Example: queue.ps1 submit -Name 'RFG smoke test' -Owner RedFactionGuerrillaRemarstered-PhysX -TimeoutMinutes 12 `
#            -File tools\run_game.ps1 -Arguments '-Name','smoke','-AutoLoadSave','-ExitAfterGameplaySeconds','45'
param(
    [string]$Name = 'run',
    [int]$ExitAfterMainMenuSeconds = 0,
    [int]$ExitAfterGameplaySeconds = 0,
    [switch]$AutoLoadSave,  # main menu -> Load Game -> most recent save
    [switch]$DebrisCollide, # experiment: all debris collides with all debris
    [switch]$Release,       # an installed release package (tools\install_package.ps1): its own ini sets the behaviour
    [int]$TimeoutSeconds = 300,
    [string[]]$Set = @(),   # plugin setting overrides: 'Section.Key=Value', e.g. 'Profiler.GameplayOnly=0'
    [switch]$ReleaseSettings,  # the release package's behaviour settings (tools\release\rfgphys.ini) on the deployed
                               # build; -Set overrides them
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered'
)

. "$PSScriptRoot\lib\common.ps1"
$root = Split-Path -Parent $PSScriptRoot

if (Get-Process -Name rfg -ErrorAction SilentlyContinue) {
    Write-Output 'rfg.exe is already running (someone playing, or another run): aborting'
    exit 3
}
if (-not (Test-Path (Join-Path $GameDir 'scripts\rfgphys.asi'))) { Write-Output 'plugin not deployed (tools\deploy.ps1)'; exit 2 }

$run = Join-Path $root ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Name")
New-Item -ItemType Directory -Force $run | Out-Null
$env:RFGPHYS_RUN_DIR = $run
$env:RFGPHYS_TEST_EXITAFTERMAINMENUSECONDS = "$ExitAfterMainMenuSeconds"
$env:RFGPHYS_TEST_EXITAFTERGAMEPLAYSECONDS = "$ExitAfterGameplaySeconds"
$env:RFGPHYS_TEST_EXITAFTERSECONDS = "$([Math]::Max(30, $TimeoutSeconds - 20))"
$env:RFGPHYS_TEST_AUTOLOADSAVE = $(if ($AutoLoadSave) { '1' } else { '0' })
$env:RFGPHYS_TEST_BLOCKSAVEWRITES = '1'   # unattended runs never write or erase save data
$env:RFGPHYS_TEST_GODMODE = '1'           # nor die (teleports, rubble, the AI); -Set 'Test.GodMode=0' to play fair
if ($Release) {
    $marker = Join-Path $GameDir 'scripts\rfgphys_installed_package.txt'
    Write-Output "release package: $(if (Test-Path $marker) { Get-Content $marker } else { 'none installed (a development deployment)' })"
} else {
    $env:RFGPHYS_EXPERIMENT_DEBRISCOLLIDE = $(if ($DebrisCollide) { '1' } else { '0' })
    $env:RFGPHYS_HARNESS_MODE = 'off'     # -Set 'Harness.Mode=record' for scripted self-test recordings
    $env:RFGPHYS_HARNESS_RECORDINGSDIR = Join-Path $root 'recordings'
}
if ($ReleaseSettings -and -not $Release) { Set-PluginOverrides (Get-ReleaseSettings $root) }
Set-PluginOverrides $Set

$savesBefore = @(Get-SaveHashes)
Backup-Saves $root $savesBefore
$knownDumps = Get-CrashDumpNames $GameDir

Set-SteamLaunchEnv $GameDir
Write-Output "starting rfg.exe, run folder $run"
# Sledge sometimes fails to create its hooks at start (sledge\sledge.log): the game never reaches its first frame. The
# failures come in streaks (on 2026-10-28, 19 of 21 starts in one job, 11 of 12 save-start replays in a row), for a
# reason inside Sledge. The game is then started again after a pause, up to 6 times, as run_replay.ps1 does; the hang
# watch ends such a start at once (about 14 s each).
for ($attempt = 1; ; $attempt++) {
    $started = Get-Date
    $p = Start-Process -FilePath (Join-Path $GameDir 'rfg.exe') -WorkingDirectory $GameDir -PassThru
    if ($p.WaitForExit($TimeoutSeconds * 1000)) {
        $code = $p.ExitCode
    } else {
        Write-Output "timeout after $TimeoutSeconds s: killing rfg.exe"
        Stop-Process -Id $p.Id -Force
        $code = 124
    }
    Write-Output "rfg.exe exit code: $code"
    Stop-StrayGame
    $sledgeError = Get-SledgeStartError $GameDir $started
    if (-not $sledgeError) { break }
    Write-Output "Sledge failed to start (the game never reached its first frame): $sledgeError"
    if ($attempt -ge 6) { break }
    Copy-NewCrashDumps $GameDir $knownDumps $run | Out-Null
    Rename-Item $run ((Split-Path $run -Leaf) + "-sledge-failed-$attempt")
    New-Item -ItemType Directory -Force $run | Out-Null
    $knownDumps = Get-CrashDumpNames $GameDir
    Start-Sleep -Seconds 10
    Write-Output 'starting the game again (the failed start''s files are in the -sledge-failed folder)'
}

Copy-NewCrashDumps $GameDir $knownDumps $run | Out-Null
foreach ($loaderLog in (Join-Path $GameDir 'dinput8.log'), (Join-Path $GameDir 'winmm.log')) {
    if (Test-Path $loaderLog) { Copy-Item $loaderLog $run }
}

$savesAfter = @(Get-SaveHashes)
if (($savesBefore -join ';') -ne ($savesAfter -join ';')) {
    Write-Output 'SAVE FILES CHANGED during the run (backups are in runs\save_backups):'
    Write-Output ('  before: ' + ($savesBefore -join '; '))
    Write-Output ('  after:  ' + ($savesAfter -join '; '))
    exit 5
}
Write-Output "save files verified untouched ($($savesAfter.Count) files)"
if ($sledgeError) {
    Write-Output "Sledge failed to start the game $attempt times in a row: the run did not happen"
    exit 6
}

$log = Join-Path $run 'rfgphys.log'
if (-not (Test-Path $log)) { Write-Output 'no rfgphys.log: the plugin did not load'; exit 2 }
Write-Output '--- rfgphys.log (tail) ---'
Get-Content $log -Tail 40
if ($code -eq 124) { exit 124 }
if (Select-String -Path $log -Pattern 'session end' -Quiet) { exit 0 }
exit 2
