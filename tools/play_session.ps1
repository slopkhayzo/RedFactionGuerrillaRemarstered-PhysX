# Queue job for an interactive play session: starts rfg.exe with the deployed plugin, waits until you quit the
# game, then writes the reports. Submitted by tools\queue_play.ps1 so it never overlaps other agents' tests.
param(
    [string]$Name = 'play',
    [int]$Minutes = 45,                # the plugin ends the game a minute before this, the queue 5 minutes after
    [Alias('WorldOnlyDebrisCollide')]
    [switch]$DebrisCollide,            # experiment: debris collides with debris (this session only)
    [int]$DebrisCollideMode = 1,       # with -DebrisCollide: 1 collide, 2 also damage and break up debris
    [switch]$Record,                   # record every Wrecking Crew match / save load as a replayable recording
    [switch]$BlockSaveWrites,          # never write saves during this session (otherwise the game saves normally)
    [string[]]$Set = @(),              # more plugin overrides: 'Section.Key=Value'
    [switch]$ReleaseSettings,          # the release package's behaviour settings (tools\release\rfgphys.ini) on the
                                       # deployed build, with -Record and -Set as usual (they override it)
    [switch]$Release,                  # an installed release package (tools\install_package.ps1) as a player has it:
                                       # no overrides, its own ini decides; its logs are copied into the run folder
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered'
)

. "$PSScriptRoot\lib\common.ps1"
$root = Split-Path -Parent $PSScriptRoot

if (Get-Process -Name rfg -ErrorAction SilentlyContinue) { Write-Output 'rfg.exe is already running: close it first'; exit 3 }
if (-not (Test-Path (Join-Path $GameDir 'scripts\rfgphys.asi'))) { Write-Output 'plugin not deployed (tools\deploy.ps1)'; exit 2 }

$run = Join-Path $root ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + "-play-$Name")
$recordings = Join-Path $root 'recordings'
New-Item -ItemType Directory -Force $run, $recordings | Out-Null
$env:RFGPHYS_TEST_EXITAFTERSECONDS = "$([Math]::Max(60, $Minutes * 60 - 60))"
if ($Release) {
    # Only the session limit above: the package's ini decides the rest, and its logs go where a player's do.
    $marker = Join-Path $GameDir 'scripts\rfgphys_installed_package.txt'
    Write-Output "release package session: $(if (Test-Path $marker) { Get-Content $marker } else { 'no package installed (a development deployment)' })"
    if ($DebrisCollide -or $Record -or $BlockSaveWrites -or $Set.Count) { Write-Output 'ignored with -Release: -DebrisCollide, -Record, -BlockSaveWrites, -Set' }
    $Record = $false
} else {
    $env:RFGPHYS_RUN_DIR = $run
    # Every behaviour switch is set explicitly, so the ini in the game folder never decides what a session runs with.
    $env:RFGPHYS_EXPERIMENT_DEBRISCOLLIDE = $(if ($DebrisCollide) { "$DebrisCollideMode" } else { '0' })
    $env:RFGPHYS_HARNESS_MODE = $(if ($Record) { 'record' } else { 'off' })
    $env:RFGPHYS_HARNESS_RECORDINGSDIR = $recordings
    $env:RFGPHYS_TEST_BLOCKSAVEWRITES = $(if ($BlockSaveWrites) { '1' } else { '0' })
    $env:RFGPHYS_TEST_AUTOLOADSAVE = '0'
    if ($ReleaseSettings) { Set-PluginOverrides (Get-ReleaseSettings $root) }
    Set-PluginOverrides $Set
    Write-Output "session settings: DebrisCollide=$env:RFGPHYS_EXPERIMENT_DEBRISCOLLIDE Harness.Mode=$env:RFGPHYS_HARNESS_MODE BlockSaveWrites=$env:RFGPHYS_TEST_BLOCKSAVEWRITES"
}

$savesBefore = @(Get-SaveHashes)
Backup-Saves $root $savesBefore
$knownDumps = Get-CrashDumpNames $GameDir
$started = Get-Date

Set-SteamLaunchEnv $GameDir
Write-Output "starting rfg.exe for your session (up to $Minutes min), run folder $run"
$p = Start-Process -FilePath (Join-Path $GameDir 'rfg.exe') -WorkingDirectory $GameDir -PassThru
$p.WaitForExit()
Write-Output "rfg.exe exited with code $($p.ExitCode)"

$crashes = Copy-NewCrashDumps $GameDir $knownDumps $run
$savesAfter = @(Get-SaveHashes)
if (($savesBefore -join ';') -ne ($savesAfter -join ';')) {
    Write-Output 'save files changed during the session (normal if the game saved; backups are in runs\save_backups)'
}

if ($Release) {
    # The session folders the plugin made under scripts\rfgphys_runs during this session.
    $sessions = @(Get-ChildItem (Join-Path $GameDir 'scripts\rfgphys_runs') -Directory -Filter '*-session' -ErrorAction SilentlyContinue |
        Where-Object { $_.CreationTime -ge $started.AddSeconds(-5) })
    foreach ($d in $sessions) { Copy-Item (Join-Path $d.FullName '*') $run }
    $log = Join-Path $run 'rfgphys.log'
    if (Test-Path $log) {
        Write-Output "logs of $($sessions.Count) session folder(s) copied to $run; from rfgphys.log:"
        Select-String -Path $log -Pattern 'HavokThreads=|PhysX host ready|PhysX host lost|WARNING|level end|pieces taken over|chunk references|session end' |
            ForEach-Object { '  ' + $_.Line } | Select-Object -First 30
    } else {
        Write-Output 'no rfgphys.log: the plugin did not run'
    }
} elseif (Test-Path (Join-Path $run 'telemetry.csv')) {
    python (Join-Path $root 'tools\profile_report.py') $run --heavy-ms 6 | Out-Null
    python (Join-Path $root 'tools\profile_report.py') $run --segment -1 --skip-s 2 --heavy-ms 6 --out report_last_round.md | Out-Null
    Write-Output "reports: $run\report.md and report_last_round.md"
    $report = Join-Path $run 'report.md'
    if (Test-Path $report) {
        Get-Content $report | Select-Object -First 30
    }
} elseif (Test-Path (Join-Path $run 'rfgphys.log')) {
    Write-Output "telemetry was off (no reports); the plugin's log is $run\rfgphys.log"
} else {
    Write-Output 'no rfgphys.log: the plugin did not run'
}
if ($Record) {
    $new = @(Get-ChildItem $recordings -Directory | Where-Object { $_.CreationTime -ge $started })
    Write-Output "recordings made in this session: $($new.Count)"
    foreach ($d in $new) {
        $manifest = Join-Path $d.FullName 'manifest.json'
        if (Test-Path $manifest) {
            $m = Get-Content $manifest -Raw | ConvertFrom-Json
            Write-Output ("  {0}: {1} frames, {2:N1} s, ended by {3}" -f $d.Name, $m.frames, $m.game_seconds, $m.end_reason)
        } else {
            Write-Output "  $($d.Name): no manifest (unfinished)"
        }
    }
}
if ($crashes -gt 0) { exit 4 }
exit 0
