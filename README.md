# rfgphys: GPU physics for Red Faction Guerrilla Re-Mars-tered

A mod for the GOG version of *Red Faction Guerrilla Re-Mars-tered* that keeps every piece of debris and keeps heavy
destruction fast. The debris is simulated by NVIDIA PhysX 5 on the graphics card, in a 64-bit process of its own, while
the game (a 32-bit program) keeps drawing it.

- Debris is never removed: buildings you bring down stay as rubble.
- Debris collides with debris and breaks it up, explosions throw piles around, and you can stand on heavy rubble while
  lighter pieces get pushed aside.
- The game's own physics (Havok) runs on several CPU cores, and heavy scenes get two rendering tweaks.
- Works in the campaign and in Wrecking Crew; never applied in multiplayer.

Tested with over 39,000 debris pieces in a single Wrecking Crew match, at about 90 FPS (1% lows about 47 FPS) on an
NVIDIA RTX 4090 with an 8-core CPU. The unmodded game has room for only about 2,000-3,000 pieces and deletes the rest.

## Install

Download a release and extract it into the game folder, the one with `rfg.exe`. The bundled `rfgphys_README.txt`
([tools/release/README.txt](tools/release/README.txt)) covers requirements, settings, uninstalling and known
limitations.

- **GOG version only.** The Steam version's `rfg.exe` is a separately compiled build that the mod doesn't support yet:
  there it stays inactive and the game runs unmodded.
- **64-bit Windows 10 or 11.** GPU physics needs an NVIDIA GTX 16 / RTX 20 series card or newer; with other graphics
  cards the debris runs on the CPU.

## How it works

- `scripts\rfgphys.asi` (32-bit, loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader))
  hooks the game's functions with [MinHook](https://github.com/TsudaKageyu/minhook), at addresses taken from the debug
  symbols (`rfg.pdb`) that ship with the GOG version. It first checks that the running `rfg.exe` is that build, and
  otherwise stays inactive.
- At start-up it launches `scripts\rfgphysx\rfgphysx_host.exe`, a 64-bit process running NVIDIA PhysX 5.12 with GPU
  rigid-body dynamics (on the CPU without a suitable GPU), at 60 steps per second. It is a separate process because the
  game is 32-bit and PhysX 5 is 64-bit only. The two share memory (a command ring and pose snapshots), and the host
  runs in a job object, so it ends with the game.
- The game's static world (terrain and buildings) is mirrored into PhysX. Debris pieces are handed over as they appear:
  their Havok bodies and game objects go, their render instances stay and follow the poses PhysX computes. Resting
  pieces are frozen until something touches them, explosions are passed on, and heavy pieces near the player get Havok
  stand-ins, so the player stands on rubble.
- The game's debris removal is switched off and its limits raised: pools for debris objects and render instances,
  Havok's heap, and the renderer's instancing capacity.

## Building from source

Requirements:

- Windows 10 or 11, 64-bit
- Visual Studio 2026 (18) Community with the *Desktop development with C++* workload and the *MSVC v143 - VS 2022
  C++ x64/x86 build tools (v14.44)* component. The scripts use the CMake that comes with Visual Studio, at its default
  install path.
- CUDA Toolkit 12.8, at its default path. PhysX's GPU module needs it, and its compiler accepts the v14.44 toolset at
  most.
- Python 3 and Git

```powershell
git clone --recurse-submodules <this repository's URL> rfgphys
cd rfgphys
tools\build_physx.cmd                                                    # PhysX 5.12 with its GPU module (CUDA kernels: takes a while)
powershell -ExecutionPolicy Bypass -File tools\build_host.ps1            # build-host\Release\rfgphysx_host.exe, PhysXGpu_64.dll
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Config Release # build\Release\rfgphys.asi
python tools\package.py                                                  # dist\rfgphys-<version>-*.zip
```

`package.py` also needs Ultimate ASI Loader 9.7.4: put `dinput8.dll` from the official
[`Ultimate-ASI-Loader.zip`](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.4) into
`third_party\ultimate-asi-loader\`; it checks the file's SHA-256. Each release's source is tagged, for example
`v0.1.0`.

- **PhysX** is a submodule, NVIDIA's repository pinned to one commit. `tools\build_physx.cmd` applies our one change
  to its build files, `third_party\physx-sdk-rfgphys.patch`, which selects the GPU architectures (GTX 16 / RTX 20 to
  RTX 50).
- **The game's addresses** are in `src/rfgphys/game/rfg_symbols.generated.h`. `tools/gen_symbols.py` generates it
  from `tools/symbols.txt` and the GOG game's `rfg.pdb`, using analysis scripts that aren't part of this repository.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/rfgphys/` | the plugin: `core/` (settings, log, main-loop hook, build check, hang watch), `hooks/`, `game/` (the game's addresses and helpers), `physx/` (the link to the host, the static-world mirror, the debris hand-over), `experiments/` (debris collision, debris limits, render tweaks, multithreaded Havok), `telemetry/`, `harness/` (record and replay for testing), `test/` (unattended-run automation) |
| `src/shared/` | the shared-memory protocol between plugin and host, the version resource |
| `host/` | the 64-bit PhysX host (`src/host_main.cpp`) and a standalone pile benchmark |
| `tools/` | build, package and test scripts; `release/` holds the release's settings and README |
| `third_party/` | MinHook, the PhysX SDK (submodule) and its patch, Ultimate ASI Loader's license ([third_party/README.md](third_party/README.md)) |

Testing and development workflow: [DEVELOPMENT.md](DEVELOPMENT.md).

## License

MIT ([LICENSE](LICENSE)). Third-party components keep their own licenses: MinHook (BSD 2-Clause), Ultimate ASI Loader
(MIT) and the NVIDIA PhysX SDK (Apache 2.0, with one modified build file); see
[third_party/README.md](third_party/README.md).
