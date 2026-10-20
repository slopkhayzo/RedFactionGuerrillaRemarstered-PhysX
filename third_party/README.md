# Third-party components

| Component | Version | Source | License | Integrity |
| --- | --- | --- | --- | --- |
| MinHook (`minhook/`) | v1.3.4 | https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip | BSD-2-Clause (`minhook/LICENSE.txt`) | archive SHA-256 `172708123daa0c98d20d3a980b16a50be14af243dc95dee6f79c24193ad010e4`; only `include/` and `src/` are kept |
| Ultimate ASI Loader, Win32 (`ultimate-asi-loader/dinput8.dll`) | v9.7.4 (git 6b44066) | https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader.zip | MIT (`ultimate-asi-loader/LICENSE.txt`, from the repository's `license`) | archive SHA-256 `952cebfc30d525afc2bdbaca954329d405ded3aa688a83027354dae14dfd5c5f`, `dinput8.dll` SHA-256 `d5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6`, Authenticode-signed |
| NVIDIA PhysX SDK (`physx-sdk/physx/`) | 5.12.0 (git `b97365c6ddb32fda8469bc788ec18024862b0368`) | https://github.com/NVIDIA-Omniverse/PhysX (partial clone, sparse checkout of `physx/`) | Apache-2.0 (`physx-sdk/physx/LICENSE.md`) | git commit; one local patch (below) |

The loader is deployed unchanged into the game folder as `dinput8.dll` by `tools/deploy.ps1`; it loads
`scripts\rfgphys.asi` when the game's own code starts and calls its exported `InitializeASI`.

PhysX is built by `tools/build_physx.cmd` into `physx-build/release-gpu/` (static libraries, `PhysXGpu.dll`). Local
patch: `physx/source/compiler/cmakegpu/CMakeLists.txt` accepts `PX_GPU_SASS_ARCHS` / `PX_GPU_PTX_ARCH`, so the GPU
kernels are compiled for chosen architectures only: by default the release set, 75, 86, 89 and 120 (GTX 16 / RTX 20
to RTX 50) with PTX 120 for newer GPUs. The modified file says so, and the change is kept as `physx-sdk-rfgphys.patch`,
which `tools/build_physx.cmd` applies to a fresh checkout.

In the repository `physx-sdk/` is a git submodule: NVIDIA's repository pinned to commit `b97365c6`. When the
repository is created, the existing clone becomes the submodule as it is:
`git submodule add https://github.com/NVIDIA-Omniverse/PhysX.git third_party/physx-sdk`. A fresh clone then needs
`git submodule update --init third_party/physx-sdk` before `tools/build_physx.cmd`.
