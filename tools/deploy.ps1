# Installs Ultimate ASI Loader (as dinput8.dll) and the rfgphys plugin into the game folder.
# Never overwrites a game file or a foreign dinput8.dll. tools\undeploy.ps1 removes everything again.
param(
    [string]$GameDir = 'D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered',
    [string]$Config = 'RelWithDebInfo',
    [switch]$ResetIni,     # rewrite scripts\rfgphys.ini with defaults
    [switch]$LoaderLog     # make Ultimate ASI Loader write dinput8.log next to the game
)

$root = Split-Path -Parent $PSScriptRoot
$ualSrc = Join-Path $root 'third_party\ultimate-asi-loader\dinput8.dll'
$ualHash = 'D5A059AA467A7A7127C8F6169F79FA63FF0F55986EE9EB2FD9A281BEBF2AA2E6'
$asi = Join-Path $root "build\$Config\rfgphys.asi"
$pdb = Join-Path $root "build\$Config\rfgphys.pdb"

if (Get-Process -Name rfg -ErrorAction SilentlyContinue) { Write-Output 'rfg.exe is running: close it first'; exit 3 }
if (-not (Test-Path (Join-Path $GameDir 'rfg.exe'))) { Write-Output "rfg.exe not found in $GameDir"; exit 2 }
if (-not (Test-Path $asi)) { Write-Output "$asi not found: build first (tools\build.ps1)"; exit 2 }

$loader = Join-Path $GameDir 'dinput8.dll'
if (Test-Path $loader) {
    $hash = (Get-FileHash $loader -Algorithm SHA256).Hash
    if ($hash -ne $ualHash) { Write-Output "a different dinput8.dll is in the game folder ($hash); not touching it"; exit 4 }
} else {
    Copy-Item $ualSrc $loader
}

$loaderIni = Join-Path $GameDir 'dinput8.ini'
if ($LoaderLog) {
    "[GlobalSets]`r`nDebugLog=1`r`n" | Set-Content -Path $loaderIni -Encoding ASCII
} elseif ((Test-Path $loaderIni) -and ((Get-Content $loaderIni -Raw) -match 'DebugLog=1')) {
    Remove-Item $loaderIni
}

$scripts = Join-Path $GameDir 'scripts'
New-Item -ItemType Directory -Force $scripts | Out-Null
# A development build replaces an installed release package (tools\install_package.ps1).
$packageMarker = Join-Path $scripts 'rfgphys_installed_package.txt'
if (Test-Path $packageMarker) { Remove-Item $packageMarker }
Copy-Item $asi $scripts -Force
if (Test-Path $pdb) { Copy-Item $pdb $scripts -Force }

# The PhysX host (64-bit, tools\build_host.ps1) and PhysX's GPU module go into scripts\rfgphysx.
$hostDir = Join-Path $root 'build-host\Release'
$hostFiles = @('rfgphysx_host.exe', 'rfgphysx_host.pdb', 'PhysXGpu_64.dll')
if (Test-Path (Join-Path $hostDir 'rfgphysx_host.exe')) {
    $pxDir = Join-Path $scripts 'rfgphysx'
    New-Item -ItemType Directory -Force $pxDir | Out-Null
    foreach ($f in $hostFiles) {
        $src = Join-Path $hostDir $f
        $dst = Join-Path $pxDir $f
        if ((Test-Path $src) -and (-not (Test-Path $dst) -or (Get-Item $src).LastWriteTimeUtc -ne (Get-Item $dst).LastWriteTimeUtc -or
                (Get-Item $src).Length -ne (Get-Item $dst).Length)) {
            Copy-Item $src $dst -Force
        }
    }
} else {
    Write-Output 'PhysX host not built (tools\build_host.ps1): Experiment.PhysX will not find it'
}

$ini = Join-Path $scripts 'rfgphys.ini'
$hangComment = '; HangSeconds: a gameplay frame still running after this many seconds while a game thread is busy ends the game with a hang report in rfgphys.log (0 = off)'
if ($ResetIni -or -not (Test-Path $ini)) {
    $runs = Join-Path $root 'runs'
    @"
; rfgphys settings. Any key can be overridden with an environment variable RFGPHYS_<SECTION>_<KEY>.
[General]
Enabled=1
OutputDir=$runs
$hangComment
HangSeconds=30

[Telemetry]
Enabled=1
ContactCounters=1

[Profiler]
; sampling profiler of the sim and render threads (about 1-2% overhead while sampling)
Enabled=1
IntervalUs=1000
GameplayOnly=1

[Test]
; > 0: quit this many seconds after the main menu first appears (unattended smoke tests)
ExitAfterMainMenuSeconds=0
; > 0: quit this many seconds after start-up (watchdog)
ExitAfterSeconds=0
"@ | Set-Content -Path $ini -Encoding ASCII
}

$experimentComment = '; DebrisCollide: 1 debris collides with debris, 2 it also damages and breaks up debris'
$solverComment = "; SolverBufferMB: with DebrisCollide, Havok's island solver buffer (with the game's 1 MB, piles of colliding debris get deleted)"
$occlusionComment = '; GpuOcclusion: 0 turns off the GPU occlusion queries (on Watchtower about 5% more FPS, p99 0.5-0.9 ms lower)'
$shadowComment = '; ShadowMinRadius*Cm: objects smaller than this radius cast no shadow into the far / mid / near cascade (0 = game default). Far 200 + Mid 50: ~8% more FPS on Watchtower, no visible change; Far 400 + Mid 100: ~11%, a few mid-distance pieces lose their shadow'
$shadowLines = @($shadowComment, 'ShadowMinRadiusFarCm=0', 'ShadowMinRadiusMidCm=0', 'ShadowMinRadiusNearCm=0')
$havokComment = "; HavokThreads: Havok steps the world on the sim thread plus this many worker threads (0 = the game's single-threaded step, at most 11; multiplayer keeps one thread). With DebrisCollide=2 on Watchtower: 3 workers +26-74% FPS, 5 workers more; without it 2 workers are enough"
$havokLines = @($havokComment, 'HavokThreads=0')
$keepComment = '; KeepDebris: 1 the game stops removing debris (no shatter-time deletions, fade-outs or load-governor deletions; single player). DebrisPool: debris piece slots (0 = the game''s 3072, up to 65000). RenderInstances: render instances for chunks and pieces (0 = the game''s 2100; the game stops creating pieces when they run out). HavokHeapMB: Havok''s heap (0 = the game''s 75 MB; colliding piles need about 15 KB per piece). ZoneReserve: object handles kept free in each world zone (a zone has 7280; below it the debris farthest from the camera is recycled)'
$keepLines = @($keepComment, 'KeepDebris=0', 'DebrisPool=0', 'RenderInstances=0', 'HavokHeapMB=0', 'ZoneReserve=1200')
$physxComment = '; PhysX: 1 starts the PhysX host (scripts\rfgphysx, GPU; CPU when CUDA is unavailable), 2 the CPU version, 0 off. PhysXRate: its fixed steps per second. PhysXTest: X0 test scene, this many bodies dropped once gameplay starts (nothing is drawn yet)'
$physxLines = @($physxComment, 'PhysX=0', 'PhysXRate=60', 'PhysXTest=0')
if (-not (Select-String -Path $ini -Pattern '^\[Experiment\]' -Quiet)) {
    Add-Content -Path $ini -Encoding ASCII -Value @"

[Experiment]
; Changes game behaviour, off by default, never applied in multiplayer. Queue sessions set it explicitly.
$experimentComment
DebrisCollide=0
$solverComment
SolverBufferMB=16
$occlusionComment
GpuOcclusion=1
$($shadowLines -join "`r`n")
$($havokLines -join "`r`n")
$($keepLines -join "`r`n")
"@
} else {
    # Older keys and comments; keep the values that were set.
    $hasSolver = Select-String -Path $ini -Pattern '^SolverBufferMB=' -Quiet
    $hasOcclusion = Select-String -Path $ini -Pattern '^GpuOcclusion=' -Quiet
    $lines = Get-Content $ini | ForEach-Object {
        if ($_ -match '^; (1: (small debris the game makes|every debris piece collides)|DebrisCollide: )') { $experimentComment }
        elseif ($_ -match '^WorldOnlyDebrisCollide=(.*)$') { "DebrisCollide=$($Matches[1])" }
        elseif ($_ -match '^; HavokThreads:') { $havokComment }
        elseif ($_ -match '^; KeepDebris:') { $keepComment }
        elseif (-not $hasSolver -and $_ -match '^DebrisCollide=') { $_; $solverComment; 'SolverBufferMB=16' }
        elseif (-not $hasOcclusion -and $_ -match '^SolverBufferMB=') { $_; $occlusionComment; 'GpuOcclusion=1' }
        else { $_ }
    }
    if (-not $hasSolver -and -not $hasOcclusion) {
        # SolverBufferMB was only added in this pass: add GpuOcclusion after it too.
        $lines = $lines | ForEach-Object { if ($_ -eq 'SolverBufferMB=16') { $_; $occlusionComment; 'GpuOcclusion=1' } else { $_ } }
    }
    if (-not ($lines -match '^ShadowMinRadiusFarCm=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^GpuOcclusion=') { $_; $shadowLines } else { $_ } }
    }
    if (-not ($lines -match '^HavokThreads=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^ShadowMinRadiusNearCm=') { $_; $havokLines } else { $_ } }
    }
    if (-not ($lines -match '^KeepDebris=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^HavokThreads=') { $_; $keepLines } else { $_ } }
    }
    if (-not ($lines -match '^RenderInstances=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^DebrisPool=') { $_; 'RenderInstances=0' } else { $_ } }
    }
    if (-not ($lines -match '^HavokHeapMB=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^RenderInstances=') { $_; 'HavokHeapMB=0' } else { $_ } }
    }
    if (-not ($lines -match '^ZoneReserve=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^HavokHeapMB=') { $_; 'ZoneReserve=1200' } else { $_ } }
    }
    if (-not ($lines -match '^PhysX=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^ZoneReserve=') { $_; $physxLines } else { $_ } }
    }
    if (-not ($lines -match '^HangSeconds=')) {
        $lines = $lines | ForEach-Object { if ($_ -match '^OutputDir=') { $_; $hangComment; 'HangSeconds=30' } else { $_ } }
    }
    $lines | Set-Content -Path $ini -Encoding ASCII
}
if (-not (Select-String -Path $ini -Pattern '^\[Harness\]' -Quiet)) {
    $recordings = Join-Path $root 'recordings'
    Add-Content -Path $ini -Encoding ASCII -Value @"

[Harness]
; off | record | replay. tools\queue_play.ps1 -Record and tools\run_replay.ps1 set it per session.
; Record: every Wrecking Crew match or save load becomes a recording; F10 ends it early, F11 adds a marker.
Mode=off
RecordingsDir=$recordings
"@
}

New-Item -ItemType Directory -Force (Join-Path $GameDir 'CrashDumps') | Out-Null
Write-Output "deployed rfgphys ($((Get-Item $asi).LastWriteTime)) and Ultimate ASI Loader to $GameDir"
exit 0
