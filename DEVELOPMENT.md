# Development

How the mod is built, tested and released on the author's machine. The scripts expect the GOG game at
`D:\Program Files\Games\GOG\Red Faction Guerrilla Re-Mars-tered` (most take `-GameDir`), and builds and game runs go
through a local test queue (`D:\Githubs\_agents\queue_executor`), so that no two runs share the machine.

## Local folders

These stay out of the repository (`.gitignore`):

| Path | Contents |
| --- | --- |
| `build/`, `build-host/` | the plugin's and the host's builds |
| `third_party/physx-build/` | PhysX's build, from `tools/build_physx.cmd` |
| `dist/` | release packages from `tools/package.py`: `rfgphys-<version>-with-asi-loader.zip`, `-plugin-only.zip`, `-symbols.zip` |
| `runs/` | one folder per game session: `rfgphys.log`, `physx_host.log`, `telemetry.csv`, `memory.csv`, `profile.bin`, `summary.json`, `report.md`; replays add `replay.json`, `checkpoints.csv`, screenshots |
| `recordings/` | one folder per recording (`manifest.json`, `input.rfgrec`, `checkpoints.csv`) |
| `research/` | notes and analysis scripts |

## Build, deploy, run

```powershell
$q = 'D:\Githubs\_agents\queue_executor\queue.ps1'
& $q submit -Name 'RFG rfgphys build' -Owner 'RedFactionGuerrillaRemarstered-PhysX' -File tools\build.ps1 -Wait
powershell -File tools\deploy.ps1            # loader as dinput8.dll + scripts\rfgphys.asi (+ rfgphys.ini on first deploy)
powershell -File tools\undeploy.ps1          # removes all of it again
```

Development builds are `RelWithDebInfo` (`tools\build.ps1` without `-Config`). Every setting is in
`src/rfgphys/core/config.h`; `scripts\rfgphys.ini` in the game folder sets them, and an environment variable
`RFGPHYS_<SECTION>_<KEY>` overrides any of them, which is how the test scripts configure a run.

An unattended run (queue job) boots the game, optionally loads the most recent save, plays idle, exits, and fails
with exit code 5 if any save file changed. Save writes are always blocked in these runs, and the real save folder is
backed up to `runs\save_backups` first.

```powershell
& $q submit -Name 'RFG gameplay smoke test' -Owner 'RedFactionGuerrillaRemarstered-PhysX' -TimeoutMinutes 12 `
    -File tools\run_game.ps1 -Arguments '-Name','smoke','-AutoLoadSave','-ExitAfterGameplaySeconds','45','-TimeoutSeconds','480'
python tools\profile_report.py runs\<run-folder>
```

`Test.ZoneCycle=1` adds a zone-streaming test to such a run: buildings near the start are blown up, then the player is
teleported away until the start's zone streams out, and back.

## Play sessions and recordings

`tools\queue_play.ps1` puts a session at the keyboard into the queue: it waits for its turn, beeps, starts the game,
and writes `report.md` and `report_last_round.md` into the session's run folder after the game is closed. Closing the
window does not cancel the session (`queue.ps1 cancel -Id <id>` does).

```powershell
powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name watchtower -Record          # make recordings
powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name watchtower-debris -DebrisCollide
powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name release -Release -Minutes 120  # the installed release package
```

With `-Record`, every Wrecking Crew match or save load in the session becomes a recording in `recordings\` (a beep
when it starts; F11 marks a moment, F10 ends it early). Options: `-Minutes 45` (the session's limit; the plugin closes
the game a minute before it), `-BlockSaveWrites`, and `-Set 'Section.Key=Value;Section.Key=Value'` for any other
setting. These sessions set the behaviour switches explicitly, except with `-Release`, where the package's own
settings decide and its logs are copied from the game folder into the run folder.

Replays (unattended): `tools\run_replay.ps1 -Recording <folder> -Repeat N -Variants 'base','debris:Experiment.DebrisCollide=1'`
as a queue job. `tools\selftest.ps1 -Build` runs the whole loop without a player: a scripted Wrecking Crew recording,
replays, comparisons and per-run totals.

## Release package

A player's package is a zip to extract into the game folder. It holds the loader, `scripts\rfgphys.asi`, the PhysX
host (`scripts\rfgphysx\`), and the release settings (`tools\release\rfgphys.ini`: everything on, diagnostics off,
Havok workers by the processor, the newest 10 session logs kept). It also holds `rfgphys_README.txt`
(`tools\release\README.txt`), the licenses and a `CrashDumps\` folder, where Ultimate ASI Loader writes crash dumps.
There are two packages: `rfgphys-<version>-with-asi-loader.zip`, and `rfgphys-<version>-plugin-only.zip` without the
loader, for games that already have one. Ultimate ASI Loader calls the plugin's `InitializeASI`, as it has since 2017.

```powershell
# as queue jobs, in this order
tools\build_physx.cmd                # PhysX GPU kernels for GTX 16 / RTX 20 to RTX 50 (sm_75, 86, 89, 120 + PTX 120)
tools\build_host.ps1
tools\build.ps1 -Config Release
python tools\package.py              # dist\: both zips (+ -symbols.zip with the matching PDBs)
tools\install_package.ps1            # replaces the development deployment with the package, as a player installs it
tools\run_game.ps1 -Release ...      # unattended runs with the package's own settings
tools\deploy.ps1 -ResetIni           # back to development builds
```

`package.py` refuses to package a GPU module that lacks kernels for one of the promised architectures (checked with
CUDA's `cuobjdump`). The version comes from `project(rfgphys VERSION ...)` in `CMakeLists.txt`. Each release's source
is tagged `v<version>`.

## Symbols

`tools/gen_symbols.py` turns the names in `tools/symbols.txt` into `src/rfgphys/game/rfg_symbols.generated.h`: RVAs,
the PDB signature, enum tables and the build identity, read from the GOG game's `rfg.pdb` with analysis scripts kept
in `research/scripts` (not part of the repository). The plugin installs no hook on an `rfg.exe` with another PDB
signature.
