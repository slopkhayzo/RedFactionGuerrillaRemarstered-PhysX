# Shared helpers for the queue job scripts (dot-source: . "$PSScriptRoot\lib\common.ps1").

$RfgSaveDir = Join-Path $env:LOCALAPPDATA 'GOG.com\Galaxy\Applications\51153410217180642\Storage\Shared\Files\autocloud\save'
$RfgSteamAppId = '667720'

# The game's save folders: GOG's, and Steam's for each Steam account on this machine (its cloud save copy).
function Get-SaveDirs {
    $dirs = [ordered]@{ 'GOG' = $RfgSaveDir }
    $userdata = 'C:\Program Files (x86)\Steam\userdata'
    if (Test-Path $userdata) {
        Get-ChildItem $userdata -Directory | ForEach-Object {
            $d = Join-Path $_.FullName "$RfgSteamAppId\remote\autocloud\save"
            if (Test-Path $d) { $dirs["Steam-$($_.Name)"] = $d }
        }
    }
    $dirs
}

# "store:name=sha256" for every file in the real save folders.
function Get-SaveHashes {
    $dirs = Get-SaveDirs
    foreach ($store in $dirs.Keys) {
        if (Test-Path $dirs[$store]) {
            Get-ChildItem $dirs[$store] -File | Sort-Object Name | ForEach-Object {
                "${store}:$($_.Name)=$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)"
            }
        }
    }
}

# Copies each save file into runs\save_backups once per distinct content.
function Backup-Saves([string]$Root, [string[]]$Hashes) {
    $backupDir = Join-Path $Root 'runs\save_backups'
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    $dirs = Get-SaveDirs
    foreach ($entry in $Hashes) {
        $file, $hash = $entry -split '=', 2
        $store, $name = $file -split ':', 2
        $prefix = if ($store -eq 'GOG') { '' } else { "$store-" }
        $copy = Join-Path $backupDir ($prefix + $hash.Substring(0, 12) + '_' + $name)
        if (-not (Test-Path $copy)) {
            Copy-Item (Join-Path $dirs[$store] $name) $copy
            Write-Output "save backed up: $copy"
        }
    }
}

# A Steam copy of the game (steam_api.dll next to rfg.exe) started directly rather than by the Steam client: with
# its app ID in the environment it runs as if Steam had started it, instead of restarting itself through Steam.
function Set-SteamLaunchEnv([string]$GameDir) {
    if (Test-Path (Join-Path $GameDir 'steam_api.dll')) {
        $env:SteamAppId = $RfgSteamAppId
        $env:SteamGameId = $RfgSteamAppId
        Write-Output "Steam copy of the game: started with SteamAppId $RfgSteamAppId"
    }
}

# After an unattended run: an rfg.exe still running was not started by the run (Steam restarted the game, so the run's
# settings and watchdogs are not in it); it is ended.
function Stop-StrayGame {
    $stray = @(Get-Process -Name rfg -ErrorAction SilentlyContinue)
    if ($stray.Count) {
        $stray | Stop-Process -Force
        Write-Output 'an rfg.exe the run did not start was still running (restarted through Steam?): ended it'
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

# The release package's behaviour settings (the [Experiment] section of tools\release\rfgphys.ini) as
# 'Experiment.Key=Value', to run a development deployment the way the package behaves.
function Get-ReleaseSettings([string]$Root) {
    $section = ''
    $settings = @()
    foreach ($line in Get-Content (Join-Path $Root 'tools\release\rfgphys.ini')) {
        if ($line -match '^\[(\w+)\]') { $section = $Matches[1]; continue }
        if ($section -eq 'Experiment' -and $line -match '^(\w+)=(.*)$') { $settings += "Experiment.$($Matches[1])=$($Matches[2])" }
    }
    return $settings
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
