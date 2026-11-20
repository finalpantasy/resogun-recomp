# Building from source

The build targets Windows 10/11 x64 with MSYS2 CLANG64 and Linux x86-64 with Clang.

## 1. Toolchain

Install [MSYS2](https://www.msys2.org) to `C:\msys64` (`winget install MSYS2.MSYS2`; another place
works with `MSYS2_ROOT` set to it), open the **MSYS2 CLANG64** shell and install the toolchain and
libraries:

```bash
pacman -S --needed git patch make \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,zlib,zydis,nasm} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
```

The build and packaging tools, `run.py`, and the tests use Python 3.10 or newer.
Install [Git for Windows](https://git-scm.com) to use Git Bash.

## 2. Source

Clone the repository with its submodules (imgui, magic_enum, miniz and xbyak, under
`gpu/third_party`):

```bash
git clone --recurse-submodules <repository URL>
```

`build.sh` fetches the submodules itself if the clone was made without them. The runner links a
small static FFmpeg 9.0.2 through `tools/build_ffmpeg.sh`. H.264 and AAC
decode movies, PNG decodes save icons, and ATRAC9 decodes Ngs2 audio. The
build uses the MP4 demuxer and the file protocol.

The script unpacks the platform archive in `prebuilt/` into `out/ffmpeg`
when its version and options match. Otherwise it downloads and verifies
FFmpeg's source against its SHA-256 before building it into that folder.
On Windows, the download uses the system curl and its certificate store when available.
Install `nasm` to enable the faster x86 assembly paths. Set
`RESOGUN_FFMPEG_PREBUILT=0` to build from source.

After changing FFmpeg's version or options, rewrite both archives and commit them with the script:
`bash tools/build_ffmpeg.sh --pack` on Windows (Git Bash or the MSYS2 shell), and
`bash tools/pack_ffmpeg_linux.sh`, which builds in an Ubuntu 24.04 container (Docker) so the Linux
libraries need no newer glibc than the release.

## 3. Build

Run from Git Bash, MSYS2, or a Linux shell.

```bash
bash build.sh
```

The build writes `out/everybodys-golf.exe` on Windows and `out/everybodys-golf` on
Linux. The unstripped binary contains debug symbols. Build failures are recorded
in `out/gpu-build.log`. On Linux, the build uses a non-PIE executable so guest
pointers can stay below 1 TiB. The Ubuntu 24.04 dependency recipe lives in
`.github/scripts/ci-deps.sh`.

```bash
bash build.sh --test
cd tests && python -m unittest discover -p 'test_*.py'
```

The test target runs the C runtime tests and GPU test binaries. Some Python
tests require a fixture or a separate decoder and skip when it is unavailable.
`build_windows.ps1 -Test` and `build_linux.sh --test` run both suites.

## 4. Prepare a dump

In a packaged copy, or with the native build's toolchain DLLs on PATH, run the
executable with `--game DIR --prepare-only`. The dump must be merged and
decrypted and contain CUSA04687 update 1.17. The launcher validates the EBOOT
hash, reads the SFO and shipped PRX modules, and writes the linked image, content
profile and ISA table without entering guest code.

The launcher remembers the dump in `paths.ini`. The image cache lives in
`game/` inside the portable root. It reuses a generation when the EBOOT, SFO, PRX
inputs and preparation version match. `--reprepare` creates a fresh generation.
Old generations can be removed while the program is closed. Keep the dump in
place so the loader can mount it as `/app0`.

`--data-dir DIR` moves portable state together. The launcher also accepts
`--config FILE`, `--save DIR`, and `--log FILE`. Relative paths on the command
line are resolved from the calling directory. Relative paths in `paths.ini`
are resolved from the portable root. Linux driver shader cache redirects point
to the portable shader directory. The Windows runner redirects its AppData environment into the portable
root. Graphics drivers may also maintain caches through Windows.

With the Golf AppContent provider, `--entitlements original` reports only
installed add-on folders. `--entitlements builtin` selects the game's
built-in entitlement table and overrides the inherited `GOLF_ENTITLEMENTS`
environment value. This setting applies to the current launch.

The launcher also accepts `--np-mode local|offline`, `--online-id NAME`
and `--discovery-port N` for local replay. They override
`GOLF_NP`, `GOLF_ONLINE_ID` and `GOLF_DISCOVERY_PORT` for the current
launch. A port of 0 disables loopback replay. Use `--font-file FILE` to
select a host font for guest text.

`--ticket-grants 1` enables synthetic ticket grants in the local WebAPI fixture.
`--tus local` enables in-process Title User Storage for the local account.
Pass `--ticket-grants 0` or `--tus off` to disable the corresponding fixture.

With the runtime login patches applied, `--local-auth 1` enables the
synthetic login probe for the local account. Use `--local-auth 0` to
override an inherited `GOLF_LOCAL_AUTH=1`.

Use `--ngs2-mix 0` to mute the sampler mix and `--ngs2-trace 1` to log
voice parameters. The launcher passes these settings to the audio runtime
through `RESOGUN_NGS2_MIX` and `RESOGUN_NGS2_TRACE`.

`--http-dump DIR` writes local replay request bodies to a capture directory.
Relative capture paths resolve under `--data-dir`. The launcher creates the directory
and passes its absolute path to the bridge through `GOLF_HTTP_DUMP`.
`--ngs2-matrix-trace 1` logs voice matrix levels. Set it to `0` to clear
an inherited `GOLF_NGS2_MATRIX_TRACE` setting.

`--fault-log 1` logs access faults on the Matching2 callback thread.
Use `--fault-log 0` to clear an inherited setting.

`python run.py prepare --game DIR --out DIR` forwards to the native preparer.
For existing developer images, `--prepared DIR --game DIR` uses their
`boot-linked.bin`, `content.bin`, and optional `isa-rewrite.bin`.

## 5. Package

```bash
python tools/package.py --zip
```

The packager checks the runner against the build receipt in `out/`. Rebuild
after switching revisions or replacing the executable.

The script stages the runner, native shared libraries, settings template, and
license. The Windows zip and Linux tar.gz contain one executable and no game
files. Use `--debug --out dist-debug` to keep debug information and include the
Windows diagnostic scripts. Stripped archives have a separate symbols archive.

On Linux, `python3 tools/package.py --appimage` uses a locally installed
`appimagetool` and writes both the tar.gz and the AppImage to `out/`. The AppImage
takes a portable data directory as its first argument.

```sh
./everybodys-golf-pc-<version>-linux-x86_64.AppImage "/path/to/golf data" --game "/path/to/dump" --prepare-only
```

Run `.github/scripts/smoke-test.sh DIR` in a fresh Linux container against an
unpacked tarball to check linker dependencies and CLI behavior without a dump.
