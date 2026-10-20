# Queue job: replays a recording unattended, once or several times, optionally for several plugin configurations
# interleaved (A, B, A, B, ...), and prints a comparison table. Each run gets its own folder under runs\.
#
#   queue.ps1 submit -Name 'RFG replay watchtower x2' -Owner RedFactionGuerrillaRemarstered-PhysX -TimeoutMinutes 30 `
#       -File tools\run_replay.ps1 -Arguments '-Recording','20261019-201500-wc-watchtower-total_chaos','-Repeat','2'
#
# -Variants takes 'label' or 'label:Section.Key=Value;Section.Key=Value', e.g. 'base', 'debris:Experiment.DebrisCollide=1'
# (as an array, or one comma-separated string when submitted through the queue).
# Exit code: 0 all runs completed (diverged or not), 3 a run did not complete, 6 the recording cannot be replayed.
param(
    [Parameter(Mandatory = $true)][string]$Recording,  # folder name under recordings\, or a full path
    [int]$Repeat = 1,
    [string[]]$Variants = @('base'),
    [string[]]$Set = @(),                               # overrides for every run
    [switch]$Report,                                    # also write report.md for each run
    [switch]$WithProfiler,                              # run the sampling profiler (it perturbs thread timing,
                                                        # and heavy-destruction replays then stop being repeatable)
    [int]$TimeoutSeconds = 0,                           # per run; 0 = derived from the recording's length
    [switch]$AdoptTrack,                                # a recording without track.bin adopts the first vanilla
                                                        # ('base') run's track; the runs after it lock to it
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered'
)

. "$PSScriptRoot\lib\common.ps1"
$root = Split-Path -Parent $PSScriptRoot
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # queue jobs pass one string

$recDir = if (Test-Path $Recording) { (Resolve-Path $Recording).Path } else { Join-Path $root "recordings\$Recording" }
$manifestPath = Join-Path $recDir 'manifest.json'
if (-not (Test-Path (Join-Path $recDir 'input.rfgrec')) -or -not (Test-Path $manifestPath)) {
    Write-Output "not a finished recording: $recDir"
    exit 6
}
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$recName = Split-Path $recDir -Leaf
Write-Output ("recording {0}: {1}, {2} frames, {3:N1} s of game time" -f $recName, $manifest.start.type, $manifest.frames, $manifest.game_seconds)

if ($manifest.start.type -eq 'save') {
    # Replays load the save from the real save folder (writes stay blocked): it must still be the recorded one.
    $copy = Join-Path $recDir $manifest.start.file
    $real = Join-Path $RfgSaveDir $manifest.start.file
    if (-not (Test-Path $copy) -or -not (Test-Path $real)) { Write-Output "save file $($manifest.start.file) missing"; exit 6 }
    if ((Get-FileHash $copy).Hash -ne (Get-FileHash $real).Hash) {
        Write-Output "the save in slot $($manifest.start.slot) changed since this recording was made: cannot replay it yet"
        exit 6
    }
}
if (Get-Process -Name rfg -ErrorAction SilentlyContinue) { Write-Output 'rfg.exe is already running: aborting'; exit 3 }
if (-not (Test-Path (Join-Path $GameDir 'scripts\rfgphys.asi'))) { Write-Output 'plugin not deployed (tools\deploy.ps1)'; exit 2 }
if ($TimeoutSeconds -le 0) { $TimeoutSeconds = [int][Math]::Max(300, 3 * [double]$manifest.game_seconds + 240) }

$savesBefore = @(Get-SaveHashes)
Backup-Saves $root $savesBefore
$results = @()
$failed = $false

for ($i = 1; $i -le $Repeat; $i++) {
    foreach ($variant in $Variants) {
        $label, $settings = $variant -split ':', 2
        $run = Join-Path $root ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + "-replay-$label-$i")
        New-Item -ItemType Directory -Force $run | Out-Null
        Get-ChildItem env: | Where-Object { $_.Name -like 'RFGPHYS_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
        $env:RFGPHYS_RUN_DIR = $run
        $env:RFGPHYS_HARNESS_MODE = 'replay'
        $env:RFGPHYS_HARNESS_RECORDING = $recDir
        $env:RFGPHYS_TEST_BLOCKSAVEWRITES = '1'
        $env:RFGPHYS_TEST_AUTOLOADSAVE = '0'
        $env:RFGPHYS_TEST_EXITAFTERGAMEPLAYSECONDS = '0'
        $env:RFGPHYS_TEST_EXITAFTERMAINMENUSECONDS = '0'
        $env:RFGPHYS_TEST_EXITAFTERSECONDS = "$($TimeoutSeconds - 20)"
        $env:RFGPHYS_EXPERIMENT_DEBRISCOLLIDE = '0'
        $env:RFGPHYS_EXPERIMENT_GPUOCCLUSION = '1'
        $env:RFGPHYS_EXPERIMENT_SHADOWMINRADIUSFARCM = '0'
        $env:RFGPHYS_EXPERIMENT_SHADOWMINRADIUSMIDCM = '0'
        $env:RFGPHYS_EXPERIMENT_SHADOWMINRADIUSNEARCM = '0'
        $env:RFGPHYS_EXPERIMENT_HAVOKTHREADS = '0'
        $env:RFGPHYS_EXPERIMENT_KEEPDEBRIS = '0'
        $env:RFGPHYS_EXPERIMENT_DEBRISPOOL = '0'
        $env:RFGPHYS_EXPERIMENT_RENDERINSTANCES = '0'
        $env:RFGPHYS_EXPERIMENT_HAVOKHEAPMB = '0'
        $env:RFGPHYS_EXPERIMENT_ZONERESERVE = '1200'
        $env:RFGPHYS_EXPERIMENT_PHYSX = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXTEST = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXTESTCRASHAFTERS = '0'
        $env:RFGPHYS_EXPERIMENT_COLLISIONSURVEY = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXWORLD = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXWORLDCHECK = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXWORLDHF = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXDEBRIS = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXDEBRISBATCH = '1'
        $env:RFGPHYS_EXPERIMENT_PHYSXSOLIDDEBRIS = '0'
        $env:RFGPHYS_EXPERIMENT_PHYSXDEBRISDROPMESHES = '1'
        $env:RFGPHYS_EXPERIMENT_PHYSXSOLIDMASSKG = '0'
        $env:RFGPHYS_HARNESS_LEAVELEVELATEND = '0'
        $env:RFGPHYS_GENERAL_HANGSECONDS = '30'
        $env:RFGPHYS_TEST_HANGAFTERSECONDS = '0'
        $env:RFGPHYS_TELEMETRY_RENDERROUTING = '0'
        $env:RFGPHYS_EXPERIMENT_UNBATCHEDCAP = '0'
        $env:RFGPHYS_EXPERIMENT_BATCHEDCAP = '0'
        $env:RFGPHYS_PROFILER_ENABLED = $(if ($WithProfiler) { '1' } else { '0' })
        Set-PluginOverrides $Set | Out-Null
        if ($settings) { Set-PluginOverrides $settings | Out-Null }

        $knownDumps = Get-CrashDumpNames $GameDir
        Set-SteamLaunchEnv $GameDir | Out-Null
        Write-Output "[$(Get-Date -Format HH:mm:ss)] run $i/$Repeat variant '$label' -> $run"
        $p = Start-Process -FilePath (Join-Path $GameDir 'rfg.exe') -WorkingDirectory $GameDir -PassThru
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
            Write-Output "  timeout after $TimeoutSeconds s: killing rfg.exe"
            Stop-Process -Id $p.Id -Force
        }
        Stop-StrayGame
        $crashes = Copy-NewCrashDumps $GameDir $knownDumps $run

        $r = [ordered]@{ variant = $label; run = $i; completed = $false; frames = 0; divergence = ''; p50 = ''; p95 = ''; p99 = ''; fps = '';
                         created = ''; destroyed = ''; island_del = ''; gov_del = ''; bodies_end = ''; pool_min_free = '';
                         not_created = ''; zone_max = ''; recycled = ''; handle_fail = ''; folder = (Split-Path $run -Leaf) }
        $resultPath = Join-Path $run 'replay.json'
        if (Test-Path $resultPath) {
            $res = Get-Content $resultPath -Raw | ConvertFrom-Json
            $r.completed = [bool]$res.completed
            $r.frames = $res.frames_replayed
            $r.divergence = if ($res.first_divergence_frame -ge 0) { "$($res.first_divergence_frame) ($($res.first_divergence_fields))" } else { 'none' }
            Write-Output ("  completed={0} frames={1}/{2} first divergence={3} checkpoints {4} compared, {5} mismatched; reason: {6}" -f `
                $res.completed, $res.frames_replayed, $res.frames_recorded, $r.divergence, $res.checkpoints_compared, $res.checkpoints_mismatched, $res.reason)
            if ($res.track_locked) {
                Write-Output ("  track: camera locked {0} frames, player put back {1} times (max {2:N2} m off); shots: {3} recorded, {4} fired and locked, {5} forced, {6} dropped, {7} missed" -f `
                    $res.track_camera_frames_locked, $res.track_player_corrections, $res.track_player_max_deviation_m, $res.track_shots_recorded, `
                    $res.track_shots_locked, $res.track_shots_forced, $res.track_shots_suppressed, $res.track_shots_missed)
            } elseif ($null -ne $res.track_locked) {
                Write-Output '  track: not locked (the recording has no track.bin)'
            }
            if ($AdoptTrack -and $label -eq 'base' -and $res.completed -and -not (Test-Path (Join-Path $recDir 'track.bin'))) {
                python (Join-Path $root 'tools\rec_tool.py') adopt-track $run | Write-Output
            }
        } else {
            Write-Output '  no replay.json: the plugin did not finish the replay'
        }
        $summaryPath = Join-Path $run 'summary.json'
        if (Test-Path $summaryPath) {
            $s = Get-Content $summaryPath -Raw | ConvertFrom-Json
            $r.p50 = $s.gameplay_frame_ms.p50; $r.p95 = $s.gameplay_frame_ms.p95; $r.p99 = $s.gameplay_frame_ms.p99; $r.fps = $s.gameplay_fps_mean
            # About 1.2 cores is the baseline while the game runs (compositor, audio, launchers); runs disturbed by
            # another heavy process have shown frame times twice as long.
            if ($null -ne $s.other_processes_cpu_cores -and $s.other_processes_cpu_cores -gt 2.5) {
                Write-Output ("  WARNING: other processes kept {0:N1} cores busy during gameplay: timings of this run are not comparable" -f $s.other_processes_cpu_cores)
            }
            $r.created = $s.gameplay_movers_created
            $r.destroyed = $s.gameplay_objects_destroyed; $r.island_del = $s.gameplay_island_watchdog_deletions
            $r.gov_del = $s.gameplay_governor_deletions
            if ($null -ne $s.last_gameplay_frame_bodies) { $r.bodies_end = "$($s.last_gameplay_frame_bodies) ($($s.last_gameplay_frame_inactive_bodies) asleep)" }
            if ($null -ne $s.debris_pool_min_free) { $r.pool_min_free = "$($s.debris_pool_min_free)/$($s.debris_pool_capacity)" }
            $r.not_created = $s.shards_not_created_pool_full
            if ($null -ne $s.physx) {
                $px = $s.physx
                $states = @('off', 'starting', 'ready', 'failed', 'lost')
                Write-Output ("  PhysX host: {0} ({1}), {2} snapshots, age {3:N2} ms mean / {4:N2} max, step {5:N2} ms mean / {6:N2} max, host work {7:N2} ms; {8} bodies max, {9} awake max; game per frame: take {10:N1} us, copy {11:N1} us (per snapshot), interpolate {12:N1} us mean / {13:N1} max; {14} commands dropped" -f `
                    $states[[int]$px.state_last], $(if ($px.gpu) { 'GPU' } else { 'CPU' }), $px.snapshots, $px.snapshot_age_ms_mean, $px.snapshot_age_ms_max,
                    $px.step_ms_mean, $px.step_ms_max, $px.host_work_ms_mean, $px.bodies_max, $px.awake_max, $px.take_us_mean,
                    $px.copy_us_mean, $px.interp_us_mean, $px.interp_us_max, $px.commands_dropped)
            }
            if ($null -ne $s.zone_handles_max_used) {
                $r.zone_max = "$($s.zone_handles_max_used)/7280"; $r.recycled = $s.keep_debris_recycled; $r.handle_fail = $s.zone_handle_failures
            }
        }
        if ($crashes -gt 0) { Write-Output "  crash reports: $crashes" }
        if (-not $r.completed) { $failed = $true }
        if ($Report -and (Test-Path (Join-Path $run 'telemetry.csv'))) {
            python (Join-Path $root 'tools\profile_report.py') $run --heavy-ms 6 | Out-Null
        }
        $results += [pscustomobject]$r
    }
}

$savesAfter = @(Get-SaveHashes)
if (($savesBefore -join ';') -ne ($savesAfter -join ';')) {
    Write-Output 'SAVE FILES CHANGED during the replays (backups are in runs\save_backups)'
    exit 5
}
Write-Output ''
Write-Output 'gameplay frame times (ms) per run:'
$results | Format-Table variant, run, completed, frames, divergence, p50, p95, p99, fps, folder -AutoSize | Out-String -Width 220 | Write-Output
Write-Output 'objects per run (gameplay): movers created; objects destroyed, of which by the island solver-memory watchdog and the load governor; dynamic bodies at the end; fewest free debris pool slots; pieces not created; most object handles used in a world zone; debris recycled to keep the zone reserve; objects created with no handle left:'
$results | Format-Table variant, run, created, destroyed, island_del, gov_del, bodies_end, pool_min_free, not_created, zone_max, recycled, handle_fail -AutoSize | Out-String -Width 240 | Write-Output
if ($failed) { exit 3 }
exit 0
