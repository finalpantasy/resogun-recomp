#!/usr/bin/env bash
# Builds out/everybodys-golf (.exe on Windows): MSYS2 CLANG64 there,
# clang and the system libraries on Linux (packages in docs/building.md). The GPU library is static
# and CMake links the loader as well. --test also runs the runtime tests.
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
case $(uname -s) in
MINGW*|MSYS*)
    # The MSYS2 CLANG64 toolchain first on PATH (MSYS2_ROOT overrides C:/msys64).
    msys2_root=${MSYS2_ROOT:-/c/msys64}
    command -v cygpath >/dev/null && msys2_root=$(cygpath -u "$msys2_root")
    if [[ -d $msys2_root/clang64/bin ]]; then
        export PATH="$msys2_root/clang64/bin:$msys2_root/usr/bin:$PATH" MSYSTEM=CLANG64
    fi
    exe=.exe
    # The Windows counterparts of the POSIX calls (src/compat_win.h).
    compat=(src/compat_win.c -lbcrypt) net=(-lws2_32) memory=(-lonecore) cflags=() ;;
Linux)
    exe=''
    # -std=c11 hides POSIX from glibc's headers.
    compat=() net=() memory=() cflags=(-D_DEFAULT_SOURCE) ;;
*) echo 'Windows (Git Bash or an MSYS2 shell) or Linux only.' >&2; exit 1 ;;
esac
jobs=${GOLF_BUILD_JOBS:-8}
revision=$(git rev-parse --verify HEAD 2>/dev/null || printf unknown)
version=$(git describe --tags --always 2>/dev/null || printf development)
launcher_cmake=$PWD/packaging/launcher.cmake
command -v cygpath >/dev/null && launcher_cmake=$(cygpath -m "$launcher_cmake")
cc=${CC:-clang} cxx=${CXX:-clang++}
for tool in "$cc" "$cxx" cmake ninja pkg-config make curl; do
    command -v "$tool" >/dev/null || { echo "Need $tool (docs/building.md)." >&2; exit 1; }
done
if [[ ! -f gpu/third_party/imgui/imgui.h || ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
    git submodule update --init --recursive
fi
# The minimal static FFmpeg in out/ffmpeg: unpacked from prebuilt/, or built when that archive is for
# another version or options (tools/build_ffmpeg.sh).
CC="$cc" bash tools/build_ffmpeg.sh out/ffmpeg
ffmpeg=$PWD/out/ffmpeg
command -v cygpath >/dev/null && ffmpeg=$(cygpath -m "$ffmpeg")
# Linux: lld when the compiler finds it (the lld package), which links the runner and the GPU tests
# much faster than GNU ld. MSYS2 CLANG64 links with lld already.
linker=()
if [[ -z $exe ]] && echo 'int main(void) { return 0; }' | "$cc" -x c - -fuse-ld=lld -o /dev/null 2>/dev/null; then
    linker=(-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld)
fi
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "${linker[@]}" \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DRESOGUN_FFMPEG_DIR="$ffmpeg" -DCMAKE_PROJECT_resogun_gpu_INCLUDE="$launcher_cmake" \
    -DGOLF_LAUNCHER_VERSION="$version" >/dev/null
if ! ninja -j "$jobs" -C out/gpu resogun > out/gpu-build.log 2>&1; then
    grep -v '^[[]' out/gpu-build.log | tail -40 >&2
    echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
cp "out/gpu/everybodys-golf$exe" out/
# Windows: the launcher window (launcher_gui/).
if [[ -n $exe ]]; then
    if ! ninja -j "$jobs" -C out/gpu golf-launcher >> out/gpu-build.log 2>&1; then
        grep -v '^[[]' out/gpu-build.log | tail -40 >&2
        echo 'Launcher build failed (full log: out/gpu-build.log)' >&2; exit 1
    fi
    cp out/gpu/EverybodysGolf.exe out/
fi
cp settings.example.ini out/
[[ -n $exe ]] && mkdir -p out/libs && cp out/gpu/libs/libs.manifest out/libs/
runner="out/everybodys-golf$exe"
checksum=$(sha256sum "$runner")
receipt="$runner.build"
printf '%s\n%s\n' "$revision" "${checksum%% *}" > "$receipt.tmp"
mv -f -- "$receipt.tmp" "$receipt"
echo "Built everybodys-golf$exe in $PWD/out"
if [[ ${1:-} == --test ]]; then
    # run_test <name> <source> [extra flags...]: builds out/<name>-test and runs it.
    run_test() {
        local name=$1 source=$2; shift 2
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${cflags[@]}" -I. -Isrc "$source" "$@" -o "out/$name-test$exe"
        "out/$name-test$exe"
    }
    run_test services tests/test_services.c "${compat[@]}"
    run_test savedata tests/test_savedata.c "${compat[@]}"
    run_test trophy-record tests/test_trophy_record.c
    run_test content tests/test_content.c "${compat[@]}"
    run_test eventflag tests/test_eventflag.c
    run_test json tests/test_json.c
    run_test ngs2 tests/test_ngs2.c -lm
    run_test wide tests/test_wide.c -lm
    run_test fiber tests/test_fiber.c
    run_test threadcalls tests/test_threadcalls.c "${compat[@]}"
    run_test net tests/test_net.c "${compat[@]}" "${net[@]}"
    run_test kernel tests/test_kernel.c "${compat[@]}"
    run_test memory tests/test_memory.c "${compat[@]}" "${memory[@]}"
    run_test memory-read-backing tests/test_memory_read_backing.c "${compat[@]}" "${memory[@]}"
    # Every *-test target of gpu/CMakeLists.txt this platform has, run from the repository root.
    mapfile -t gpu_tests < <(ninja -C out/gpu -t targets all | sed -n 's/^\([a-z0-9-]*-test\): .*/\1/p' | sort -u)
    if ! ninja -j "$jobs" -C out/gpu "${gpu_tests[@]}" >> out/gpu-build.log 2>&1; then
        grep -v '^[[]' out/gpu-build.log | tail -40 >&2
        echo 'Test build failed (full log: out/gpu-build.log)' >&2; exit 1
    fi
    # The video core's user folder (caches, logs) in out/, not ~/.local/share/shadPS4; it must
    # exist, or Linux falls back to the home folder.
    mkdir -p out/gpu-test-user
    export RESOGUN_GPU_USER_DIR=out/gpu-test-user
    for test in "${gpu_tests[@]}"; do
        args=()
        [[ $test == isa-stub-test ]] && args=(tests/data/isa_fixture.bin)
        if ! "out/gpu/$test$exe" "${args[@]}" > "out/$test.log" 2>&1; then
            tail -20 "out/$test.log" >&2
            echo "$test failed (log: out/$test.log)" >&2; exit 1
        fi
        echo "$test: ok"
    done
fi
