# Queue job: runs rfgphysx_bench (build-host\Release) over a matrix of backends and piece counts, samples GPU load
# with nvidia-smi meanwhile, and writes everything to runs\<time>-physx-bench\ (summary.csv, one steps CSV per
# run, gpu.csv). Prints the summary table at the end.
#   -Runs 'gpu:20000','cpu8:20000:--settled'   (backend[threads]:pieces[:bench options]; default: the matrix below)
param(
    [string[]]$Runs = @('gpu:5000', 'gpu:10000', 'gpu:20000', 'gpu:50000', 'gpu:100000',
                        'cpu8:5000', 'cpu8:10000', 'cpu8:20000', 'cpu8:50000', 'cpu4:10000', 'cpu16:20000'),
    [string[]]$Extra = @()   # more options for every run, e.g. '--sites','8'
)

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-host\Release\rfgphysx_bench.exe'
if (-not (Test-Path $exe)) { Write-Output "not built: $exe (tools\build_host.ps1)"; exit 2 }
$Runs = @($Runs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$out = Join-Path $root ('runs\' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-physx-bench')
New-Item -ItemType Directory -Force $out | Out-Null
$summary = Join-Path $out 'summary.csv'

$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
$gpuLog = Join-Path $out 'gpu.csv'
$sampler = $null
if ($smi) {
    $sampler = Start-Process -FilePath $smi.Source -ArgumentList '--query-gpu=timestamp,utilization.gpu,memory.used,power.draw,clocks.sm',
        '--format=csv,noheader', '-lms', '250', '-f', $gpuLog -PassThru -WindowStyle Hidden
}

$failed = $false
foreach ($run in $Runs) {
    $backend, $pieces, $flags = $run -split ':', 3
    $name = ($run -replace '[: ]+', '-') -replace '[^A-Za-z0-9_.-]', ''
    $benchArgs = @('--pieces', $pieces, '--summary', $summary, '--label', $run, '--csv', (Join-Path $out "steps-$name.csv"))
    if ($backend -eq 'gpu') { $benchArgs += @('--backend', 'gpu') }
    else { $benchArgs += @('--backend', 'cpu', '--threads', ($backend -replace 'cpu', '')) }
    if ($flags) { $benchArgs += @($flags -split ' ' | Where-Object { $_ }) }
    $benchArgs += $Extra
    $start = Get-Date
    Write-Output "[$(Get-Date -Format HH:mm:ss)] $run"
    & $exe @benchArgs 2>&1 | ForEach-Object { "  $_" } | Write-Output
    if ($LASTEXITCODE -ne 0) { Write-Output "  exit code $LASTEXITCODE"; $failed = $true }
    "$run,$($start.ToString('o')),$((Get-Date).ToString('o'))" | Add-Content (Join-Path $out 'runs.csv')
}
if ($sampler) { Stop-Process -Id $sampler.Id -Force -ErrorAction SilentlyContinue }

Write-Output ''
Write-Output "results: $out"
if (Test-Path $summary) {
    Import-Csv $summary | Format-Table label, phase, step_mean_ms, step_p95_ms, step_max_ms, awake_mean, awake_max, pairs_mean,
        frozen_mean, freeze_ms_mean, readback_mean_ms, cpu_cores_mean, ms_per_1000_awake -AutoSize | Out-String -Width 240 | Write-Output
}
if ($failed) { exit 3 }
exit 0
