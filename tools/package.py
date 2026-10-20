"""Make the release package: a zip to extract into the game folder, with the release settings (tools/release).

python tools/package.py [--config Release]

Build first, through the shared queue: tools/build_physx.cmd (GPU kernels for the release set), tools/build_host.ps1,
tools/build.ps1 -Config Release. Writes into dist/:
  rfgphys-<version>/                             the package's files (extract into the folder with rfg.exe)
  rfgphys-<version>-with-asi-loader.zip          all of it
  rfgphys-<version>-plugin-only.zip              without Ultimate ASI Loader, for games that already have one
  rfgphys-<version>-symbols.zip                  the PDBs and binaries they belong to, for crash reports
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
UAL_SHA256 = 'd5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6'  # Ultimate ASI Loader 9.7.4
CUOBJDUMP = r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\cuobjdump.exe'
GPU_ARCHS = ['sm_75', 'sm_86', 'sm_89', 'sm_120']  # what the README promises: GTX 16 / RTX 20 to RTX 50

CRASHDUMPS_README = """While this folder exists, Ultimate ASI Loader writes a crash dump here when the game crashes (about 60 MB
each). Delete the folder to turn that off.
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def zip_dir(src, dst, skip=()):
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, dirs, files in os.walk(src):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(base, name)
                rel = os.path.relpath(full, src).replace(os.sep, '/')
                if rel not in skip:
                    z.write(full, rel)


def fail(message):
    print(message)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', default='Release')
    args = ap.parse_args()

    with open(os.path.join(ROOT, 'CMakeLists.txt'), encoding='utf-8') as f:
        version = re.search(r'project\(rfgphys VERSION ([0-9.]+)', f.read()).group(1)
    name = f'rfgphys-{version}'
    dist = os.path.join(ROOT, 'dist')
    out = os.path.join(dist, name)

    build = os.path.join(ROOT, 'build', args.config)
    host = os.path.join(ROOT, 'build-host', 'Release')
    files = {
        'asi': os.path.join(build, 'rfgphys.asi'),
        'asi_pdb': os.path.join(build, 'rfgphys.pdb'),
        'host': os.path.join(host, 'rfgphysx_host.exe'),
        'host_pdb': os.path.join(host, 'rfgphysx_host.pdb'),
        'gpu': os.path.join(host, 'PhysXGpu_64.dll'),
        'ual': os.path.join(ROOT, 'third_party', 'ultimate-asi-loader', 'dinput8.dll'),
    }
    for path in files.values():
        if not os.path.exists(path):
            fail(f'missing: {path} (build first)')
    if not os.path.exists(os.path.join(ROOT, 'LICENSE')):
        fail('missing: LICENSE, the project\'s own license (MIT)')
    if sha256(files['ual']) != UAL_SHA256:
        fail('dinput8.dll is not the known Ultimate ASI Loader 9.7.4')

    # The GPU module must carry compiled kernels for every architecture the README promises.
    if os.path.exists(CUOBJDUMP):
        listing = subprocess.run([CUOBJDUMP, '-lelf', files['gpu']], capture_output=True, text=True).stdout
        found = sorted(set(re.findall(r'\bsm_\d+\b', listing)), key=lambda s: int(s[3:]))
        missing = [a for a in GPU_ARCHS if a not in found]
        print(f'PhysXGpu_64.dll kernels: {", ".join(found) or "none"}')
        if missing:
            fail(f'PhysXGpu_64.dll has no kernels for {", ".join(missing)}: rebuild PhysX (tools/build_physx.cmd)')
    else:
        print('cuobjdump not found: the GPU architectures are not checked')

    if os.path.exists(out):
        shutil.rmtree(out)
    scripts = os.path.join(out, 'scripts')
    for d in (os.path.join(scripts, 'rfgphysx'), os.path.join(out, 'rfgphys_licenses'), os.path.join(out, 'CrashDumps')):
        os.makedirs(d)
    shutil.copy2(files['ual'], out)
    shutil.copy2(files['asi'], scripts)
    with open(os.path.join(ROOT, 'tools', 'release', 'rfgphys.ini'), encoding='ascii') as f:
        ini = f.read()
    with open(os.path.join(scripts, 'rfgphys.ini'), 'w', encoding='ascii', newline='\r\n') as f:
        f.write(ini)
    shutil.copy2(files['host'], os.path.join(scripts, 'rfgphysx'))
    shutil.copy2(files['gpu'], os.path.join(scripts, 'rfgphysx'))
    with open(os.path.join(ROOT, 'tools', 'release', 'README.txt'), encoding='utf-8') as f:
        readme = f.read().replace('@VERSION@', version)
    with open(os.path.join(out, 'rfgphys_README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(readme)
    with open(os.path.join(out, 'CrashDumps', 'README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(CRASHDUMPS_README)
    licenses = {
        'rfgphys-MIT.txt': 'LICENSE',
        'Ultimate-ASI-Loader-MIT.txt': os.path.join('third_party', 'ultimate-asi-loader', 'LICENSE.txt'),
        'MinHook-BSD-2-Clause.txt': os.path.join('third_party', 'minhook', 'LICENSE.txt'),
        'NVIDIA-PhysX-SDK-Apache-2.0.txt': os.path.join('third_party', 'physx-sdk', 'physx', 'LICENSE.md'),
    }
    for dst, src in licenses.items():
        shutil.copy2(os.path.join(ROOT, src), os.path.join(out, 'rfgphys_licenses', dst))

    # Two archives: everything, and the same without the loader for games that already have one.
    full = os.path.join(dist, f'{name}-with-asi-loader.zip')
    plugin_only = os.path.join(dist, f'{name}-plugin-only.zip')
    zip_dir(out, full)
    zip_dir(out, plugin_only, skip={'dinput8.dll', 'rfgphys_licenses/Ultimate-ASI-Loader-MIT.txt'})
    single = os.path.join(dist, f'{name}.zip')  # the one archive earlier builds made
    if os.path.exists(single):
        os.remove(single)

    symbols_dir = os.path.join(dist, f'{name}-symbols')
    if os.path.exists(symbols_dir):
        shutil.rmtree(symbols_dir)
    os.makedirs(symbols_dir)
    for key in ('asi', 'asi_pdb', 'host', 'host_pdb'):
        shutil.copy2(files[key], symbols_dir)
    symbols = os.path.join(dist, f'{name}-symbols.zip')
    zip_dir(symbols_dir, symbols)
    shutil.rmtree(symbols_dir)

    print(f'{name} (plugin-only: all but dinput8.dll and its license):')
    for base, _, names in os.walk(out):
        for n in sorted(names):
            p = os.path.join(base, n)
            print(f'  {os.path.relpath(p, out):<45} {os.path.getsize(p):>12,} bytes')
    for package in (full, plugin_only):
        print(f'{os.path.basename(package)}: {os.path.getsize(package):,} bytes, SHA-256 {sha256(package)}')
    print(f'symbols: {symbols} ({os.path.getsize(symbols):,} bytes)')


if __name__ == '__main__':
    main()
