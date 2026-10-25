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

With another mod's `dinput8.dll` in the game folder (Sledge, a mod manager, loads through one too, but only Sledge),
`deploy.ps1` and `install_package.ps1` put the loader in as `winmm.dll`, which the game loads as well, and leave the
other `dinput8.dll` alone.

Development builds are `RelWithDebInfo` (`tools\build.ps1` without `-Config`). Every setting is in
`src/rfgphys/core/config.h`; `scripts\rfgphys.ini` in the game folder sets them, and an environment variable
`RFGPHYS_<SECTION>_<KEY>` overrides any of them, which is how the test scripts configure a run.

An unattended run (queue job) boots the game, optionally loads the most recent save, plays idle, exits, and fails
with exit code 5 if any save file changed. Save writes are always blocked in these runs, and the real save folder is
backed up to `runs\save_backups` first. Such a run ends by itself with a hang report when a gameplay frame runs for
30 s or a menu or loading frame for 60 s, and a timed one (`-ExitAfterGameplaySeconds`) 5 s into the death menu.

```powershell
& $q submit -Name 'RFG gameplay smoke test' -Owner 'RedFactionGuerrillaRemarstered-PhysX' -TimeoutMinutes 12 `
    -File tools\run_game.ps1 -Arguments '-Name','smoke','-AutoLoadSave','-ExitAfterGameplaySeconds','45','-TimeoutSeconds','480'
python tools\profile_report.py runs\<run-folder>
```

`Test.ZoneCycle=1` adds a zone-streaming test to such a run: buildings near the start are blown up, then the player is
teleported away until the start's zone streams out, and back (held in the air after each hop until the ground there
has streamed in, then landed). With `Experiment.PhysXWorldCheck=1` the world mirror is checked again after the first
landing, among the zones that just streamed in, and 10 s after the player is back (`world mirror check` lines,
`world_mirror_check_2.csv` and `_3.csv`): the ground that streamed in must match Havok's. `Test.ZoneTour=N` hops N times
across the open world instead, every 15 s, to a loaded zone picked at random (the same sequence each run), and blows
up the buildings near every landing; with `Experiment.PhysXWorldCheck=1` the mirror is checked after each landing. The world mirror's 5 s lines count the bodies that
came at the address of one that had just left (`... added (N at the address of one that left)`); before the mirror
told bodies apart by `hkpEntity::m_uid`, each of those left the old geometry in the host and never sent the new.

`Test.VehiclePush=1` (campaign, with `-AutoLoadSave`, 75 s of gameplay): 15 s in, a building next to the nearest
vehicle is blown up; 20 s later the vehicle is moved through the rubble, and the log compares how many pieces near its
path moved with how many farther off did.

`Test.RestoreBuilding=1` (campaign, with `-AutoLoadSave`, 60 s of gameplay): 15 s in, the biggest standing building
10-60 m away is blown up; 12 s later the game restores it, as an activity does when it starts over
(`world::restore_object`), and 5 s after that the log says how many of the pieces that broke off it are left in Havok
and in PhysX and whether it stands again (`restore building: ... passed`). `Test.RestoreBuilding=N` does it N times
(about 21 s each), as a demolition replayed over and over, and compares the general movers and the free render
instances after the first and the last cycle.

`Test.ViewShots=1` (campaign, with `-AutoLoadSave`, 60 s of gameplay) is for comparing settings on screen: from 10 s in,
a screenshot a second for 40 s (`view_NN.png`), the camera turned about 36 degrees every 4 s, and the frame times of
those 40 s in a histogram (`view shots: ... frames`). `Test.ViewShots=N` (N > 1) runs N seconds, in bursts of four shots
half a second apart every 10 s (`view_BBB_K.png`), for long stays such as a `Test.ZoneTour`.

`-ReleaseSettings` on `run_game.ps1`, `run_replay.ps1` and `queue_play.ps1` runs the deployed build with the release
package's behaviour settings (the `[Experiment]` section of `tools\release\rfgphys.ini`); unlike `-Release` it keeps
`-Record` and `-Set`, which override it.

`Test.SolverGuard=1` checks, at start-up, the guard on Havok's solver setup (`hkSolverBuildJacobianFromAtomsNotContact`
loops for good on a constraint atom it cannot get past; a tower shattered into thousands of pieces hung the game
there): four such atom lists go through it on a thread of their own, and the log says whether they came back
(`Test.SolverGuard: passed`). In play, `rfgphys.log` reports such atoms (`Havok solver: ...`; ten times, then every
thousandth).

`Test.HavokAudit=1` checks the island totals Havok's solver lays its buffer out by (`hkpSimulationIsland`'s
constraint count and schema and temporary sizes) against the sums over each island's constraints, before every
island's solve, after the multithreaded jobs and after every step, along with each constraint's owner and whether its
bodies are in another island. Every collide task's hand-off (what it gathered for its island) is checked against how
much its agents' contact constraints really moved, a task that starts after its island's post-collide job is
reported, and island merges asked for inside the jobs are logged. Concurrent solves are checked for shared solver
memory, and a crash inside the solver logs where in its island's buffer it was and writes `havok_crash_island.txt`
and `havok_crash_solver.bin`. Lines start with `Havok audit`; a summary comes every 30 s. `Test.HavokAudit=2` also
repairs islands whose totals fall short of their constraints before they are solved. `Test.HavokAuditJobs=1` adds
checks after each island split, around each broadphase job and after each post-collide job (with what the replay was
about to add and remove), and keeps the last steps of the islands of 1,000+ bodies in memory, job by job; the first
time such an island is wrong after the jobs, they go to `havok_island_history.txt`. `Test.HavokJobsOnSimThread=1` runs
every job of the multithreaded step on the sim thread: Havok's multithreaded pipeline in one order, without races.
The multithreaded corruption these found (an island past 65,535 sectors of agents, which Havok's collide jobs cannot
count, lost a step's contact changes; `experiments/havok_mt.cpp` splits such jobs) reproduces with the tower
recording `20261023-183208-newgame`, `Experiment.PhysX=0;Experiment.PhysXWorld=0;Experiment.PhysXDebris=0` (all
debris stays in Havok), `Harness.ReplayRealTime=1` and multithreaded Havok; `rfgphys.log` then says
`experiment HavokThreads: an island of ... bodies has ... narrowphase agent sectors ...: its job split in 2`.

`Test.AutoNewGame=1` starts a new game without input: New Game in the main menu, the autosave warning accepted,
normal difficulty, no golden hammer, and the intro skipped as a key press does (save writes stay blocked, as in every
unattended run). With `Harness.Mode=record` a new game is recorded from its intro (start type `new_game`, whether the
menus were driven or chosen by hand in `queue_play.ps1 -Record`), and its replays drive the same menus.
`Test.Visit=x,z,tx,tz` takes the player to (x, z) facing (tx, tz), hopping through loaded zones and landing once the
ground has streamed in; `|x,z,tx,tz@s` adds stops nearby at s seconds of gameplay, and the log lists the buildings
around the first. `Test.ResetDestruction=1` makes `-AutoLoadSave` load with the world's destruction reset. The first
mission's tower: `Test.AutoNewGame=1;Test.Visit=-2242.2,-2279,-2246.3,-2284.4` with an input script swinging there.
`Harness.ReplayPasses=2` plays a replay that ends at the main menu again in the same process (a second game after the
first level ended, as in a play session that goes back to the menu); the earlier passes keep their outputs as
`replay_pass1.json`, `checkpoints_pass1.csv` and so on, and `replay.json` is the last pass's. `Harness.ReplayRealTime=1`
keeps live play's pace: each replayed frame takes at least as long as it did when recorded (replays otherwise run as
fast as the machine allows, since the frame cap cannot work on the virtual clock). Recordings reach the disk
about every quarter second (input, track and checkpoints), so `tools\rec_tool.py salvage` keeps almost all of a
session that crashed, track included.

`Test.StandInProbe=1` checks the pieces' Havok stand-ins every 2 s: a ray with the game's default filter (the one
melee and bullets use) through a piece that has one, and a hammer-like blow through the zone object's
`process_melee_hit`; the log says whether the ray found the stand-in and whether the piece moved in the host.

`physx_host.log` sums up every 5 s: bodies, awake and frozen ones (frozen as calm or as going nowhere), what woke them,
the restless pieces (awake for over 5 s without a break: a pile that jitters), how many pieces were put back on the
ground after something pressed them through it, how deep the pieces coming over overlap what is already there (1 in 32
measured with PhysX's exact penetration, split by what they overlap) and how many were pushed upward in their first
second (bubbling; landings and tumbles count too), and the melee sweeps (blows wider than the game's own, from mods)
with the pieces they pushed. `rfgphys.log` ends with how many pieces fell through the world anyway (given up below
-400 m).

`Test.InspectFrames=4400,5000` (replays; pair it with `Harness.ScreenshotFrames`) describes the view at those replay
frames in the log: what the game's default ray, its destroyable-geometry ray and a fan of rays around the view hit
(object, motion, pieces left in a building), the pieces in PhysX near the hit and along the view ray (frozen, awake or
asleep, and any the host has no body for), the Havok bodies around, and the view ray against the static world in the
host.

A Wrecking Crew match without anyone at the keyboard: `Harness.Mode=record;Harness.AutoStart=wc` with
`Harness.AutoStartOptions=<hex>` (the match setup, as `options_hex` in a recording's manifest) and
`Harness.InputScript=...\tools\input_scripts\start_turn.txt` (starts the turn, then stands still for 40 s). For
example Watchtower in Behemoth mode, with the civilian walker:
`04000000010000000100000000000000040000000000000000000000010000000200000000010000FFFFFFFF`. `Test.FlyCamera=1` adds a scripted flight of the fly
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
