@echo off
rem Builds PhysX 5.12 (third_party\physx-sdk, pinned commit b97365c6) for the 64-bit PhysX host and benchmark:
rem static CPU libraries plus PhysXGpu_64.dll (the SDK's supported pairing), static CRT (/MT), PhysX's "release"
rem configuration, with Ninja. Output: third_party\physx-build\release-gpu\.
rem
rem CUDA 12.8's nvcc accepts host compilers up to Visual Studio 2022, so the whole PhysX build uses the v143
rem toolset (14.44) that Visual Studio 2026 installs; the host, built with v145, links the libraries.
rem %1: GPU architectures with compiled kernels (default 75,86,89,120: GTX 16 / RTX 20, RTX 30, RTX 40, RTX 50, the
rem release set); %2: the architecture of the PTX that newer GPUs JIT (default 120); "sdk" = the SDK's own list (70 to
rem 120), much slower to build. A GPU older than every listed one gets no GPU PhysX (the host then uses CPU PhysX).
rem CPU-heavy and long (CUDA kernels): run it through the shared queue. Re-running rebuilds only what changed;
rem other architectures rebuild the GPU module.
setlocal
set "ARCHS=%~1"
if "%ARCHS%"=="" set "ARCHS=75,86,89,120"
set "PTX=%~2"
if "%PTX%"=="" set "PTX=120"
for /f "tokens=1 delims=," %%a in ("%ARCHS%") do set "BASE=%%a"
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"
if not exist "%CUDA%\bin\nvcc.exe" (
    echo CUDA 12.8 not found at %CUDA%
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul || exit /b 1
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%CUDA%\bin;%PATH%"
set "CUDA_PATH=%CUDA%"
cd /d "%~dp0.."
if not exist "third_party\physx-sdk\physx\CMakeLists.txt" (
    echo PhysX source missing: git submodule update --init third_party/physx-sdk
    exit /b 1
)
rem Our one change to PhysX (third_party\physx-sdk-rfgphys.patch), applied to a fresh checkout of the submodule.
git -C third_party\physx-sdk apply --reverse --check ..\physx-sdk-rfgphys.patch >nul 2>&1
if errorlevel 1 (
    git -C third_party\physx-sdk apply ..\physx-sdk-rfgphys.patch || exit /b 1
    echo applied third_party\physx-sdk-rfgphys.patch
)
set "OUT=%CD%\third_party\physx-build\release-gpu"
set "ARCHDEFS=-DPX_GPU_SASS_ARCHS=%ARCHS% -DPX_GPU_PTX_ARCH=%PTX% -DCMAKE_CUDA_ARCHITECTURES=%BASE%"
if "%ARCHS%"=="sdk" set "ARCHDEFS=-UPX_GPU_SASS_ARCHS -UPX_GPU_PTX_ARCH -DCMAKE_CUDA_ARCHITECTURES=70"
echo PhysX GPU architectures: %ARCHS% (PTX %PTX%)
rem Configured every time, so that other architectures take effect.
"%CMAKE%" -S third_party\physx-sdk\physx -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=release ^
    -DPX_GENERATE_GPU_PROJECTS=ON -DPX_GENERATE_STATIC_LIBRARIES=ON -DPX_GENERATE_GPU_STATIC_LIBRARIES=OFF ^
    -DCMAKE_CUDA_COMPILER="%CUDA%\bin\nvcc.exe" -DCUDAToolkit_ROOT="%CUDA%" %ARCHDEFS% ^
    -DNV_USE_STATIC_WINCRT=ON -DNV_USE_DEBUG_WINCRT=OFF ^
    -DPX_BUILDSNIPPETS=OFF -DPX_BUILDPVDRUNTIME=OFF -DPX_FLOAT_POINT_PRECISE_MATH=OFF ^
    -DPX_OUTPUT_LIB_DIR="%OUT%\lib" -DPX_OUTPUT_BIN_DIR="%OUT%\bin" -DPX_OUTPUT_DLL_DIR="%OUT%\bin" ^
    -DCMAKE_INSTALL_PREFIX="%OUT%\install" || exit /b 1
"%CMAKE%" --build "%OUT%" || exit /b 1
"%CMAKE%" --install "%OUT%" || exit /b 1
echo built PhysX with GPU support into %OUT%
dir /s /b "%OUT%\install\*.lib" "%OUT%\install\*.dll" "%OUT%\install\*PxConfig.h" 2>nul
exit /b 0
