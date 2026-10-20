# rfgphys: Red Faction Guerrilla Re-Mars-tered physics project

An ASI plugin (loaded by Ultimate ASI Loader) for the GOG build of *Red Faction Guerrilla Re-Mars-tered*, aimed at
making heavy destruction scale on modern hardware. Research and plan: [research/docs](research/docs/README.md).

Current state: plugin foundations, telemetry (with an object creation/fade/deletion census), sampling profiler and
the record/replay harness work. Behaviour changes are opt-in experiments: colliding debris (`Experiment.DebrisCollide`,
with `Experiment.SolverBufferMB` so the game stops deleting piles of colliding debris), render-thread savings
(`Experiment.GpuOcclusion`, `Experiment.ShadowMinRadius*Cm`) and Havok's own multithreaded simulation
(`Experiment.HavokThreads`, [01 §10.8](research/docs/01-findings.md#108-havoks-own-multithreading-the-phase-4-spike-2026-10-19))
and no debris removal at all (`Experiment.KeepDebris` with its limits, [01 §10.9](research/docs/01-findings.md#109-every-way-the-game-removes-debris-and-the-limits-behind-them-2026-10-19);
the world's 7,280 object handles per zone are kept from running out by `Experiment.ZoneReserve`, [01 §10.10](research/docs/01-findings.md#1010-kept-debris-in-play-the-worlds-own-object-limits-2026-10-19)).
The PhysX side so far: PhysX 5.12 built with its GPU code and a standalone pile benchmark
([06 §12](research/docs/06-physx-experiment.md#12-measured-physx-512-on-the-rtx-4090-with-piles-of-debris-2026-10-19)).

## Layout

| Path | Contents |
| --- | --- |
| `src/rfgphys/` | the plugin: `core/` (config, log, session, main-loop hook, contact-listener hook, game-callback lock, build check), `hooks/`, `game/` (generated symbols), `telemetry/` (frame timers, object census, profiler), `harness/` (record/replay, checkpoints, random-generator guard, screenshots), `test/` (unattended-run automation, save-write blocker), `experiments/` (debris collision, render tweaks, multithreaded Havok) |
| `tools/` | `gen_symbols.py` + `symbols.txt`, `build.ps1`, `deploy.ps1` / `undeploy.ps1`, `run_game.ps1` (unattended queue job), `run_replay.ps1` (replays, A/B variants), `queue_play.ps1` + `play_session.ps1` (your play sessions and recordings through the queue), `profile_report.py`, `hang_report.py` (resolves a multithreaded-Havok hang report), `rec_tool.py`, `build_physx.cmd` + `build_host.ps1` + `run_physx_bench.ps1` (PhysX build and pile benchmark, queue jobs), `input_scripts/`, `lib/common.ps1` |
| `host/` | the 64-bit side of the PhysX experiment: `bench/rfgphysx_bench.cpp` (pile benchmark) for now; built by `tools/build_host.ps1` into `build-host/` |
| `third_party/` | MinHook source, the Ultimate ASI Loader binary (with versions and hashes), the PhysX SDK (`physx-sdk/`, a sparse clone pinned to commit `b97365c6`, one local build patch) and its build (`physx-build/`, from `tools/build_physx.cmd`) |
| `research/` | docs, analysis scripts, generated analysis data (`research/out`, regenerable) |
| `recordings/` | one folder per recording (`manifest.json`, `input.rfgrec`, `checkpoints.csv`) |
| `dist/` | release packages from `tools/package.py`: `rfgphys-<version>-with-asi-loader.zip`, `-plugin-only.zip`, `-symbols.zip` |
| `runs/` | one folder per game session: `rfgphys.log`, `telemetry.csv`, `memory.csv`, `profile.bin`, `summary.json`, `report.md`; replays add `replay.json`, `checkpoints.csv`, `fingerprints.csv`, screenshots |

## Build, deploy, run

Builds and game runs go through the shared queue (`D:\Githubs\_agents\queue_executor`, see its `AGENTS.md`).

```powershell
$q = 'D:\Githubs\_agents\queue_executor\queue.ps1'
& $q submit -Name 'RFG rfgphys build' -Owner 'RedFactionGuerrillaRemarstered-PhysX' -File tools\build.ps1 -Wait
powershell -File tools\deploy.ps1            # loader as dinput8.dll + scripts\rfgphys.asi (+ rfgphys.ini on first deploy)
powershell -File tools\undeploy.ps1          # removes all of it again
```

Unattended run (queue job): boots the game, optionally loads the most recent save, plays idle, exits, and fails
with exit code 5 if any save file changed. Save writes are always blocked in these runs, and the real save folder is
backed up to `runs\save_backups` first.

```powershell
& $q submit -Name 'RFG gameplay smoke test' -Owner 'RedFactionGuerrillaRemarstered-PhysX' -TimeoutMinutes 12 `
    -File tools\run_game.ps1 -Arguments '-Name','smoke','-AutoLoadSave','-ExitAfterGameplaySeconds','45','-TimeoutSeconds','480'
python tools\profile_report.py runs\<run-folder>
```

Playing yourself through the queue (so your session never overlaps another agent's test): `tools\queue_play.ps1`
waits for your turn, beeps, starts the game, and writes `report.md` and `report_last_round.md` into the session's
run folder after you quit. Closing the window does not cancel the session (`queue.ps1 cancel -Id <id>` does).

```powershell
powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name watchtower -Record          # make recordings
powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name watchtower-debris -DebrisCollide
powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name watchtower-base              # game defaults
```

With `-Record`, every Wrecking Crew match or save load in the session becomes a recording in `recordings\` (a beep
when it starts; F11 marks a moment, F10 ends it early); see [04](research/docs/04-test-harness.md#4-recording-workflow-you).
Options: `-Minutes 45` (session limit; the plugin closes the game a minute before it), `-BlockSaveWrites`,
`-Set 'Section.Key=Value;Section.Key=Value'` for any other setting. Queue sessions set the experiment and harness
switches explicitly; playing without the queue uses `scripts\rfgphys.ini` in the game folder (any key can also be
overridden with an environment variable `RFGPHYS_<SECTION>_<KEY>`) and still records a session under `runs\`.

Replaying (agent, unattended): `tools\run_replay.ps1 -Recording <folder> -Repeat N -Variants 'base','debris:Experiment.DebrisCollide=1'`
as a queue job; see [04 §5](research/docs/04-test-harness.md#5-unattended-replay-agent). `tools\selftest.ps1 -Build`
runs the whole loop without a user (scripted Wrecking Crew recording, replays, comparisons, per-run totals).

## Release package

A player's package is a zip to extract into the game folder: the loader, `scripts\rfgphys.asi`, the PhysX host
(`scripts\rfgphysx\`), the release settings (`tools\release\rfgphys.ini`: everything on, diagnostics off, Havok
workers by the processor, the newest 10 session logs kept), `rfgphys_README.txt` (`tools\release\README.txt`), the
third-party licenses and an empty-looking `CrashDumps\` (Ultimate ASI Loader writes crash dumps into it). It comes
twice: `rfgphys-<version>-with-asi-loader.zip`, and `rfgphys-<version>-plugin-only.zip` without the loader for games
that already have one (Ultimate ASI Loader calls the plugin's `InitializeASI`, as it has since 2017).

```powershell
# as queue jobs, in this order
tools\build_physx.cmd                # PhysX GPU kernels for GTX 16 / RTX 20 to RTX 50 (sm_75, 86, 89, 120 + PTX 120)
tools\build_host.ps1
tools\build.ps1 -Config Release
python tools\package.py              # dist\: both zips (+ -symbols.zip with the matching PDBs)
tools\install_package.ps1            # replaces the development deployment with the package, as a player installs it
tools\run_game.ps1 -Release ...      # unattended runs with the package's own settings
powershell -ExecutionPolicy Bypass -File tools\queue_play.ps1 -Name release -Release -Minutes 120   # your session
tools\deploy.ps1 -ResetIni           # back to development builds
```

`package.py` refuses to package a GPU module that lacks kernels for one of the promised architectures (checked with
`cuobjdump`). The version comes from `project(rfgphys VERSION ...)` in `CMakeLists.txt`.

## License

MIT (`LICENSE`). Third-party components keep their own licenses: MinHook (BSD 2-Clause), Ultimate ASI Loader (MIT)
and the NVIDIA PhysX SDK (Apache 2.0, with one modified build file); see `third_party/README.md`.

## Symbols

`tools/gen_symbols.py` turns the names in `tools/symbols.txt` into `src/rfgphys/game/rfg_symbols.generated.h`
(RVAs, PDB signatures, enum tables, build identity) using the analysis scripts in `research/scripts`. The plugin
refuses to install hooks on any other `rfg.exe` build.
