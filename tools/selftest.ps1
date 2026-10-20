# Queue job: harness self-test without a user. Deploys the current build, records a scripted Wrecking Crew match
# (tools\input_scripts\look_and_fire.txt), replays it -Repeat times with the same diagnostics, and compares the
# replays with the recording and with each other.
#
#   queue.ps1 submit -Name 'RFG harness self-test' -Owner RedFactionGuerrillaRemarstered-PhysX -TimeoutMinutes 25 `
#       -File tools\selftest.ps1 -Arguments '-Repeat','2'
param(
    [int]$Repeat = 2,
    [string]$Script = '',
    [string]$Screenshots = '1700,2400,3900,6000',
    [switch]$NoTrace,
    [switch]$Build,        # build first (tools\build.ps1)
    [switch]$RealInput,    # record from the real keyboard and mouse (nobody touching them) instead of a script
    [int]$MaxFrames = 1500,  # with -RealInput: recording length
    [string]$InputFrom = '',  # play another recording's setup and input frames instead of a script (real time)
    [string[]]$Variants = @('base'),  # replay variants, as in run_replay.ps1
    [string[]]$Set = @()   # extra overrides for the recording and the replays: 'Section.Key=Value;...'
)

$root = Split-Path -Parent $PSScriptRoot
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })  # queue jobs pass one string
if (-not $Script) { $Script = Join-Path $root 'tools\input_scripts\look_and_fire.txt' }
elseif (-not (Test-Path $Script)) { $Script = Join-Path $root "tools\input_scripts\$Script" }
$trace = if ($NoTrace) { '0' } else { '1' }
$extra = ($Set | Where-Object { $_ }) -join ';'

if ($Build) {
    & (Join-Path $PSScriptRoot 'build.ps1') | Select-String -Pattern 'warning|error|rfgphys.asi'
    if ($LASTEXITCODE -ne 0) { Write-Output 'build failed'; exit $LASTEXITCODE }
}
& (Join-Path $PSScriptRoot 'deploy.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$before = @(Get-ChildItem (Join-Path $root 'recordings') -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
$recordSet = "Harness.Mode=record;Harness.AutoStart=wc;Harness.TraceRng=$trace;Harness.ScreenshotFrames=$Screenshots"
if ($InputFrom) {
    $source = if (Test-Path $InputFrom) { (Resolve-Path $InputFrom).Path } else { Join-Path $root "recordings\$InputFrom" }
    $recordSet += ";Harness.InputFromRecording=$source;Harness.AutoStartFrom=$source"
} elseif ($RealInput) {
    $recordSet += ";Harness.MaxFrames=$MaxFrames"
} else {
    $recordSet += ";Harness.InputScript=$Script"
}
if ($extra) { $recordSet += ";$extra" }
Write-Output "=== recording (scripted, no user input)"
& (Join-Path $PSScriptRoot 'run_game.ps1') -Name 'selftest-record' -TimeoutSeconds 360 -Set $recordSet | Select-Object -Last 4
$recording = Get-ChildItem (Join-Path $root 'recordings') -Directory | Where-Object { $before -notcontains $_.Name } |
    Sort-Object Name | Select-Object -Last 1
if (-not $recording) { Write-Output 'no recording was made'; exit 2 }
Write-Output "recording: $($recording.Name)"

Write-Output "=== replays"
$replaySet = "Harness.TraceRng=$trace;Harness.ScreenshotFrames=$Screenshots"
if ($extra) { $replaySet += ";$extra" }
$started = Get-Date
& (Join-Path $PSScriptRoot 'run_replay.ps1') -Recording $recording.Name -Repeat $Repeat -Set $replaySet -Variants $Variants
$replayExit = $LASTEXITCODE

Write-Output "=== comparisons"
$runs = @(Get-ChildItem (Join-Path $root 'runs') -Directory | Where-Object { $_.Name -like '*-replay-*' -and $_.CreationTime -ge $started } |
    Sort-Object Name)
$tool = Join-Path $root 'tools\rec_tool.py'
foreach ($r in $runs) {
    Write-Output "--- recording vs $($r.Name)"
    python $tool compare $recording.FullName $r.FullName
    if (-not $NoTrace) { python $tool tracediff $recording.FullName $r.FullName }
}
if ($runs.Count -ge 2) {
    Write-Output "--- $($runs[0].Name) vs $($runs[1].Name)"
    python $tool compare $runs[0].FullName $runs[1].FullName
    if (-not $NoTrace) { python $tool tracediff $runs[0].FullName $runs[1].FullName }
}
Write-Output "=== gameplay totals"
$recordRun = Get-ChildItem (Join-Path $root 'runs') -Directory | Where-Object { $_.Name -like '*-selftest-record' } |
    Sort-Object Name | Select-Object -Last 1
python $tool stats $recordRun.FullName @($runs | ForEach-Object { $_.FullName })
exit $replayExit
