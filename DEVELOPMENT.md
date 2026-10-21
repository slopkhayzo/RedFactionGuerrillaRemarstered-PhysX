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
teleported away until the start's zone streams out, and back. `Test.FlyCamera=1` adds a scripted flight of the fly
camera (below), 15 s into gameplay: screenshots `fly_0_game.png` to `fly_5_control.png` in the run folder, and checks
in the log (`fly camera test: passed`; with a message box on screen the game ignores the mouse, and the input check is
reported as not verified).

## Fly camera

For screenshots: with `[FlyCamera] Enabled=1` (off by default, in the release settings too), F10 (`Key`, a virtual-key
code) takes the view off the game camera in single player. W/S/A/D move along the view, Space/Ctrl along its up
vector, Q/E roll, the mouse or the arrow keys turn about the view's own axes, Shift moves 4x faster and Alt 4x slower,
the wheel zooms and the middle button resets the zoom. `Speed` (m/s) and `LookSpeed` (percent) set the rates. The game
gets no input while flying, so the player stands still, and the HUD is hidden (`HideHud=0` keeps it; message boxes
such as tutorial hints stay, so close them first). F10 again gives the view back to the game camera, which never
moved; leaving gameplay (a menu, loading, death) does too.

Only the drawn view changes (`render_pos` and `render_orient` of `Rfg_camera`, written in `game_render_setup_frame`,
and its `real_fov` while that runs), so the game camera's state stays with the player, and so does world streaming:
far from the player, places show their distant versions. It is not installed with the record/replay harness, which
uses F10 too. The remaster has a free camera of its own (`s_freeCamera`, `overwriteFreeCamera` in `render_frame`,
moved by `DebugAxisInput`), but nothing turns it on.

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

## Symbols and builds

`tools/gen_symbols.py` turns the names in `tools/symbols.txt` into `src/rfgphys/game/rfg_symbols.generated.h`: each
function's and global's RVA in the GOG and the Steam build, function sizes, enum tables and both builds' identities.
The GOG addresses come from the GOG game's `rfg.pdb`, read with analysis scripts kept in `research/scripts` (not part
of the repository).

The Steam `rfg.exe` was compiled from the same source 77 seconds after the GOG one, without symbols. Its code is a
little different (1.9 KB more, functions shifted unevenly, data one page later). `tools/match_builds.py` finds each GOG
function in it: the function's bytes, with absolute addresses (from the relocation table) and branches leaving the
function left out, searched near where the previous function was found. Then it pairs the data the matched code
refers to. Of 126,396 functions 126,337 match; the 59 others are platform code (options and controller menus,
multiplayer sessions, start-up), none of them used here. The result is `tools/steam_map.json`, which
`gen_symbols.py` turns into the Steam column; it stops if any address cannot be mapped.

At start-up the plugin recognizes the build by the PDB GUID in its debug directory and switches every address to
that build's (`rfg::select_build`), so code reads addresses from `rfg::rva` when it runs, never into constants.
Places inside functions are written as function + offset (`rfg::CodeSite`). On Steam, diagnostics report code
addresses as the GOG build's (hang reports, the profiler, the object census, the replay harness's random-number
streams), so the GOG PDB still names them and recordings replay alike on both builds.

```powershell
python tools\match_builds.py        # tools\steam_map.json, from both rfg.exe (default install paths)
python tools\gen_symbols.py         # the header
```

Test runs on the Steam copy: `-GameDir 'D:\Program Files\Games\Steam\steamapps\common\Red Faction Guerrilla Re-MARS-tered'`
on `run_game.ps1`, `run_replay.ps1`, `install_package.ps1`, `deploy.ps1` and `queue_play.ps1`. The game is started with
its Steam app ID in the environment, so it doesn't restart itself through the Steam client. Save backups and checks
cover the Steam save folder too.
