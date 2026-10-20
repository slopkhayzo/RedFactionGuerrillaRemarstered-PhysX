# Shared helpers for the queue job scripts (dot-source: . "$PSScriptRoot\lib\common.ps1").

$RfgSaveDir = Join-Path $env:LOCALAPPDATA 'GOG.com\Galaxy\Applications\51153410217180642\Storage\Shared\Files\autocloud\save'

# "name=sha256" for every file in the real save folder.
function Get-SaveHashes {
    if (Test-Path $RfgSaveDir) {
        Get-ChildItem $RfgSaveDir -File | Sort-Object Name | ForEach-Object {
            "$($_.Name)=$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)"
        }
    }
}

# Copies each save file into runs\save_backups once per distinct content.
function Backup-Saves([string]$Root, [string[]]$Hashes) {
    $backupDir = Join-Path $Root 'runs\save_backups'
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    foreach ($entry in $Hashes) {
        $name, $hash = $entry -split '=', 2
        $copy = Join-Path $backupDir ($hash.Substring(0, 12) + '_' + $name)
        if (-not (Test-Path $copy)) {
            Copy-Item (Join-Path $RfgSaveDir $name) $copy
            Write-Output "save backed up: $copy"
        }
    }
}

# Plugin setting overrides for this run: 'Section.Key=Value' -> environment variable RFGPHYS_SECTION_KEY.
# Several settings can share one string separated by ';' (queue jobs can pass a parameter only once).
function Set-PluginOverrides([string[]]$Settings) {
    foreach ($s in ($Settings | ForEach-Object { $_ -split ';' } | Where-Object { $_ })) {
        if ($s -match '^(\w+)\.(\w+)=(.*)$') {
            Set-Item -Path ("env:RFGPHYS_" + $Matches[1].ToUpper() + "_" + $Matches[2].ToUpper()) -Value $Matches[3]
            Write-Output "setting $($Matches[1]).$($Matches[2]) = $($Matches[3])"
        } else {
            Write-Output "ignoring malformed setting '$s'"
        }
    }
}

# Names of the crash reports currently in the game's CrashDumps folder.
function Get-CrashDumpNames([string]$GameDir) {
    $dir = Join-Path $GameDir 'CrashDumps'
    if (Test-Path $dir) { @(Get-ChildItem $dir | Select-Object -ExpandProperty Name) } else { @() }
}

# Copies crash reports that appeared during the run into the run folder; returns how many.
function Copy-NewCrashDumps([string]$GameDir, [string[]]$Known, [string]$RunDir) {
    $dir = Join-Path $GameDir 'CrashDumps'
    $count = 0
    if (Test-Path $dir) {
        Get-ChildItem $dir | Where-Object { $Known -notcontains $_.Name } | ForEach-Object {
            Copy-Item $_.FullName $RunDir
            Write-Host "crash report collected: $($_.Name)"
            $count++
        }
    }
    return $count
}
