#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package the single Everybody's Golf runner and its native dependencies.

Build with bash build.sh. --zip writes the Windows zip or Linux tar.gz.
On Linux, --appimage also writes an AppImage using a locally installed appimagetool.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile

from source_identity import source_identity

PORT = Path(__file__).resolve().parent.parent
LINUX = sys.platform.startswith('linux')
EXES = ('everybodys-golf',) if LINUX else ('everybodys-golf.exe',)
# Linux libraries the host provides: bundled, they break its GPU drivers, display connection or audio
# (libstdc++ too: Mesa needs the one it was built with, at least as new as this build's).
HOST_LIBS = re.compile(r'^(linux-vdso|ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl'
                       r'|libstdc\+\+|libgcc_s|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*'
                       r'|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor'
                       r'|libXrandr|libwayland-.*|libasound|libudev|libpulse.*|libfontconfig|libfreetype'
                       r'|libnvidia-.*|libcuda|libz)\.so')
APPIMAGE = PORT / 'packaging' / 'appimage'
# The glitch kit that --debug ships in diagnose/.
DIAGNOSE = PORT / 'packaging' / 'diagnose'
# libs/libs.manifest in a release: res/libs.manifest's assembly, with the DLLs beside it.
LIBS_MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="libs" version="1.0.0.0" processorArchitecture="amd64"/>
{files}</assembly>
"""

README = '''Everybody's Golf for PC
Release {release}

Use a merged, decrypted dump of CUSA04687 with update 1.17.
The dump folder must contain eboot.bin, sce_module and sce_sys.
Keep the dump available for the runner to mount as /app0.

  {exe} --game "path to dump" --prepare-only
  {exe}

Run --help for options. The first command remembers the dump, builds the
linked image and exits. Later starts reuse the image when its inputs match.
Use --reprepare to rebuild and --dlc DIR for extracted add-on folders.
--entitlements original limits AppContent to installed add-on folders
when the Golf AppContent provider is present.

By default the executable's folder holds game/, save/, shaders/, gpu/, log/
and paths.ini. Windows also directs AppData from the runner process into host/.
--data-dir DIR moves that portable state together.
--config FILE, --save DIR and --log FILE override individual paths.
The Windows graphics driver may maintain its own shader cache in LocalAppData.

On Linux, give the AppImage one data directory before any options. For example:
  ./EverybodyGolf.AppImage "path to data" --game "path to dump" --prepare-only
The Linux tar.gz may be extracted and run directly.

Release prerequisites are an x86-64 host with AVX and Vulkan 1.3.
See LICENSE.txt for the source license (GPL-2.0-or-later).
{debug_note}'''
DEBUG_NOTE = '''
To collect local diagnostics, start the game with diagnose\\start.bat on Windows.
The log and frame captures go into diagnose\\.
'''


def pe_imports(path):
    """The DLLs a PE file imports, normal and delay-loaded."""
    data = path.read_bytes()
    fh = struct.unpack_from('<I', data, 0x3C)[0] + 4
    sections, opt_size = struct.unpack_from('<H', data, fh + 2)[0], struct.unpack_from('<H', data, fh + 16)[0]
    opt = fh + 20
    if struct.unpack_from('<H', data, opt)[0] != 0x20B:
        raise SystemExit(f'{path}: not a 64-bit PE file')
    table = opt + opt_size

    def offset(rva):
        for i in range(sections):
            vsize, va, raw, ptr = struct.unpack_from('<4I', data, table + i * 40 + 8)
            if va <= rva < va + max(vsize, raw):
                return ptr + rva - va
        raise SystemExit(f'{path}: bad import table')

    names = []
    # Import descriptors are 20 bytes with the name at +12; delay-load descriptors 32 with it at +4.
    for directory, size, name_at in ((1, 20, 12), (13, 32, 4)):
        rva = struct.unpack_from('<I', data, opt + 112 + directory * 8)[0]
        if not rva:
            continue
        at = offset(rva)
        while (name := struct.unpack_from('<I', data, at + name_at)[0]):
            start = offset(name)
            names.append(data[start:data.index(b'\0', start)].decode('ascii'))
            at += size
    return names


def dll_closure(exe, bin_dir):
    """The DLLs `exe` needs from MSYS2, and theirs. What Windows has (System32, API sets) stays
    Windows': vulkan-1.dll is the driver's, as when run.py starts the game, also on a build machine
    without a GPU driver (CI), where System32 has none."""
    system = Path(os.environ.get('SYSTEMROOT', r'C:\Windows')) / 'System32'
    queue, seen, found, missing = list(pe_imports(exe)), set(), [], []
    while queue:
        name = queue.pop(0)
        key = name.lower()
        if (key in seen or key.startswith(('api-ms-', 'ext-ms-')) or key == 'vulkan-1.dll'
                or (system / name).exists()):
            continue
        seen.add(key)
        source = bin_dir / name
        if not source.is_file():
            missing.append(name)
            continue
        found.append(source)
        queue += pe_imports(source)
    if missing:
        raise SystemExit(f'Not in {bin_dir}: {", ".join(missing)} (packages in docs/building.md)')
    return found


def linux_tool(*names):
    for name in names:
        if (path := shutil.which(name)):
            return path
    raise SystemExit(f'Need {names[0]} (.github/scripts/ci-deps.sh installs it).')


def so_closure(exes):
    """The shared libraries `exes` load, theirs included (ldd lists them all), less the host's."""
    found, missing = {}, []
    for exe in exes:
        listing = subprocess.run(['ldd', str(exe)], capture_output=True, text=True, check=True).stdout
        for line in listing.splitlines():
            match = re.fullmatch(r'\s*(\S+)\s+=>\s+(.+?)\s*', line)
            if not match:
                continue
            name, resolution = match.groups()
            if HOST_LIBS.match(name):
                continue
            if resolution == 'not found':
                missing.append(name)
            else:
                # Keep spaces in the full path and retain its lookup name as the archive name.
                path = re.sub(r'\s+\(0x[0-9a-fA-F]+\)$', '', resolution)
                found[name] = Path(path)
    if missing:
        raise SystemExit(f'ldd cannot find {", ".join(sorted(set(missing)))}')
    return dict(sorted(found.items()))


def set_rpath(path, rpath):
    """Bundled libraries find each other through their own RUNPATH: the exe's applies to its direct
    dependencies only."""
    subprocess.run([linux_tool('patchelf'), '--set-rpath', rpath, str(path)], check=True)


def min_glibc(paths):
    """The newest GLIBC_ symbol version the files use: the oldest glibc they start on."""
    objdump = shutil.which('objdump') or shutil.which('llvm-objdump-18')
    if not objdump:
        return None
    versions = set()
    for path in paths:
        dump = subprocess.run([objdump, '-T', str(path)], capture_output=True, text=True).stdout
        versions |= set(re.findall(r'GLIBC_(2\.[0-9.]+)', dump))
    return max(versions, key=lambda v: tuple(map(int, v.split('.'))), default=None)


def git(*args):
    return subprocess.run(['git', *args], cwd=PORT, capture_output=True, text=True).stdout.strip()


def release_name(product='everybodys-golf-pc'):
    """Name artifacts after the source revision."""
    described = git('describe', '--tags', '--always', '--dirty') or 'unknown'
    when = git('log', '-1', '--format=%cs') or time.strftime('%Y-%m-%d')
    return f'{product}-{described}-{when}'


def verify_build(executable, revision):
    """Verify the build receipt before using the checkout revision in the archive name."""
    rebuild = 'Run bash build.sh before packaging.'
    receipt = executable.with_name(f'{executable.name}.build')
    try:
        lines = receipt.read_text(encoding='ascii').splitlines()
    except (OSError, UnicodeError):
        raise SystemExit(f'Cannot read build receipt for {executable.name}. {rebuild}') from None
    if len(lines) != 3 or lines[0] != revision:
        raise SystemExit(f'{executable.name} was built from a different revision. {rebuild}')
    if lines[2] != source_identity(PORT):
        raise SystemExit(f'Source files changed after building {executable.name}. {rebuild}')
    digest = hashlib.sha256()
    with executable.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    if lines[1] != digest.hexdigest():
        raise SystemExit(f'{executable.name} differs from its build receipt. {rebuild}')


def copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def check_output(path, *, directory=False):
    """Reject output paths that could redirect a write outside the package."""
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    reparse = getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    if directory:
        valid = stat.S_ISDIR(info.st_mode)
    else:
        valid = stat.S_ISREG(info.st_mode) and info.st_nlink == 1
    if reparse or not valid:
        raise SystemExit(f'Cannot write package output at {path}. Choose a fresh --out directory.')


def check_dependencies(out, folder, expected):
    deps = out / folder
    check_output(deps, directory=True)
    expected = {os.path.normcase(name) for name in expected}
    if deps.is_dir():
        for entry in deps.iterdir():
            if os.path.normcase(entry.name) not in expected:
                raise SystemExit(f'{entry} is outside this package. Choose a fresh --out directory.')


def strip(source, target, bin_dir, mode='--strip-all'):
    """Copies an exe without its debug info (RelWithDebInfo DWARF is most of the build size).
    Crash logs give exe+offset, which the unstripped build resolves."""
    target.parent.mkdir(parents=True, exist_ok=True)
    tool = linux_tool('llvm-strip-18', 'llvm-strip', 'strip') if LINUX else str(bin_dir / 'llvm-strip.exe')
    subprocess.run([tool, mode, '-o', str(target), str(source)], check=True)


def appimage(out, shipped, release, tarball):
    if not tarball.is_file() or not tarball.stat().st_size:
        raise SystemExit('The Linux tarball must exist before building its AppImage.')
    builder = linux_tool('appimagetool')
    image = tarball.parent / f'{release}-linux-x86_64.AppImage'
    with tempfile.TemporaryDirectory(prefix='golf-appimage-', dir=image.parent) as temp:
        root = Path(temp) / 'Golf.AppDir'
        for name in shipped:
            copy(out / name, root / name)
        for name in ('AppRun', 'everybodys-golf.desktop', 'everybodys-golf.svg'):
            copy(APPIMAGE / name, root / name)
        (root / 'AppRun').chmod(0o755)
        (root / '.DirIcon').symlink_to('everybodys-golf.svg')
        staged = Path(temp) / image.name
        command = [builder]
        if runtime := os.environ.get('APPIMAGE_RUNTIME_FILE'):
            command += ['--runtime-file', runtime]
        subprocess.run(command + [str(root), str(staged)], env={**os.environ, 'ARCH': 'x86_64'}, check=True)
        if not staged.is_file() or not staged.stat().st_size:
            raise SystemExit('appimagetool produced no AppImage.')
        staged.replace(image)
    return image


def write_archive(path, entries):
    if LINUX:
        # tar keeps the executable bits a zip would lose.
        with tarfile.open(path, 'w:gz', compresslevel=9) as archive:
            for source, name in entries:
                archive.add(source, name)
    else:
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for source, name in entries:
                archive.write(source, name)


def archive_bundle(out, shipped, release, *, debug, include_appimage):
    destination = PORT / 'out'
    name = f'{release}-linux' if LINUX else release
    extension = '.tar.gz' if LINUX else '.zip'
    # Finish every artifact before replacing any previous archive.
    with tempfile.TemporaryDirectory(prefix='golf-archives-', dir=destination) as temp:
        stage = Path(temp)
        player = stage / f'{name}{"-debug" if debug else ""}{extension}'
        write_archive(player, ((out / path, f'{release}/{path}') for path in sorted(shipped)))
        artifacts = [(player, '')]
        if not debug:
            symbols = stage / f'{name}-symbols{extension}'
            write_archive(symbols, ((destination / exe, exe) for exe in EXES))
            artifacts.append((symbols, ' (not for players)'))
        if include_appimage:
            artifacts.append((appimage(out, shipped, release, player), ''))

        for item, _ in artifacts:
            check_output(destination / item.name)
        for item, note in artifacts:
            published = item.replace(destination / item.name)
            print(f'{published}: {published.stat().st_size / 2**20:.0f} MiB{note}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', type=Path, help='the release folder (default dist). Use a fresh folder when dependencies change.')
    p.add_argument('--appimage', action='store_true', help='also build the Linux AppImage and tarball')
    p.add_argument('--zip', action='store_true', help='also write out/<release>.zip (and <release>-symbols.zip)')
    p.add_argument('--debug', action='store_true', help='keep debug info in the exes and ship the diagnose/ kit')
    a = p.parse_args()
    bin_dir = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
    if a.appimage and (not LINUX or a.debug):
        p.error('--appimage requires a Linux player build')
    programs = {exe: PORT / 'out' / exe for exe in EXES}
    revision = git('rev-parse', '--verify', 'HEAD') or 'unknown'
    for name, executable in programs.items():
        if not executable.is_file():
            raise SystemExit(f'out/{name} not found: build first (bash build.sh).')
        verify_build(executable, revision)
    out = (a.out or PORT / 'dist').resolve()
    if out == (PORT / 'out').resolve():
        raise SystemExit('Build output is the package source. Choose a different --out directory.')
    check_output(out, directory=True)
    if any((out / old).exists() for old in ('resogun.exe', 'resogun', 'resogun-setup.exe', 'resogun-setup', 'prepare')):
        raise SystemExit(f'{out} contains a legacy release. Choose a fresh --out directory.')
    assets = {
        'settings.example.ini': PORT / 'settings.example.ini',
        'LICENSE.txt': PORT / 'LICENSE',
        'Orbitron-OFL.txt': PORT / 'gpu/third_party/fonts/orbitron/OFL.txt',
        'recomplauncher.json': PORT / 'recomplauncher.json',
    }
    outputs = [*programs, *assets, 'release.txt', 'README.txt']
    if LINUX:
        libs = so_closure(list(programs.values()))
        check_dependencies(out, 'lib', libs)
        outputs.extend(f'lib/{name}' for name in libs)
    else:
        if next(out.glob('*.dll'), None) is not None:
            raise SystemExit(f'{out} contains loose DLLs. Choose a fresh --out directory.')
        dlls = dll_closure(programs[EXES[0]], bin_dir)
        check_dependencies(out, 'libs', ['libs.manifest', *(dll.name for dll in dlls)])
        outputs.extend(f'libs/{dll.name}' for dll in dlls)
        outputs.append('libs/libs.manifest')
        if a.debug:
            check_output(out / 'diagnose', directory=True)
            for bat in sorted(DIAGNOSE.glob('*.bat')):
                name = f'diagnose/{bat.name}'
                assets[name] = bat
                outputs.append(name)
    for name in outputs:
        check_output(out / name)
    release = release_name()
    shipped = []

    def ship(source, name):
        copy(source, out / name)
        shipped.append(name)

    for name, source in programs.items():
        if a.debug:
            ship(source, name)
        else:
            strip(source, out / name, bin_dir)
            shipped.append(name)
        if LINUX:
            set_rpath(out / name, '$ORIGIN/lib')
    if LINUX:
        for name, lib in libs.items():
            target = out / 'lib' / name
            if a.debug:
                copy(lib, target)
            else:
                strip(lib, target, bin_dir, '--strip-unneeded')
            set_rpath(target, '$ORIGIN')
            shipped.append(f'lib/{name}')
        dlls = libs
    else:
        for dll in dlls:
            ship(dll, f'libs/{dll.name}')
        files = ''.join(f'  <file name="{dll.name}"/>\n' for dll in dlls)
        (out / 'libs').mkdir(exist_ok=True)
        (out / 'libs' / 'libs.manifest').write_bytes(LIBS_MANIFEST.format(files=files).replace('\n', '\r\n').encode())
        shipped.append('libs/libs.manifest')

    (out / 'release.txt').write_bytes(f'{release}\n'.encode())
    readme = README.format(release=release, exe=EXES[0], debug_note=DEBUG_NOTE if a.debug and not LINUX else '')
    (out / 'README.txt').write_bytes(readme.encode())
    for name, source in assets.items():
        ship(source, name)
    shipped += ['release.txt', 'README.txt']
    size = sum((out / name).stat().st_size for name in shipped)
    if LINUX:
        glibc = min_glibc([out / name for name in shipped if name in programs or name.startswith('lib/')])
        print(f'{out}: {release}{" (debug)" if a.debug else ""}, {len(dlls)} libraries, {len(shipped)} files, '
              f'{size / 2**20:.0f} MiB, needs glibc {glibc or "?"}')
    else:
        print(f'{out}: {release}{" (debug)" if a.debug else ""}, {len(dlls)} DLLs, {len(shipped)} files, {size / 2**20:.0f} MiB')

    if a.zip or a.appimage:
        archive_bundle(out, shipped, release, debug=a.debug, include_appimage=a.appimage)
    return 0


if __name__ == '__main__':
    sys.exit(main())
