#!/usr/bin/env bash
# Check the unpacked Linux runner without a game dump or a GPU.
# Usage: smoke-test.sh <unpacked release folder>
set -euo pipefail
release=$(readlink -f -- "$1")
if command -v apt-get >/dev/null; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends libvulkan1 \
        libx11-6 libx11-xcb1 libxcb1 libxcb-render0 libxcb-shm0 libxext6 libxfixes3 libxrender1 libxi6 libxcursor1 libxrandr2 \
        libwayland-client0 libwayland-egl1 libwayland-cursor0 libxcb-shape0 libxcb-xfixes0 libasound2t64 libpulse0 libudev1 \
        libfontconfig1 libfreetype6 zlib1g libdrm2 libgbm1 libegl1 libgl1 libstdc++6 >/dev/null
elif command -v dnf >/dev/null; then
    dnf install -y -q vulkan-loader libX11 libX11-xcb libxcb libXext libXfixes libXrender libXi \
        libXcursor libXrandr libwayland-client libwayland-egl libwayland-cursor alsa-lib pulseaudio-libs \
        systemd-libs fontconfig freetype zlib-ng-compat libdrm mesa-libgbm libglvnd-egl libglvnd-glx \
        libstdc++ >/dev/null
elif command -v pacman >/dev/null; then
    # -Syu, not -Sy: the image lags the repos, and a partial upgrade can leave new packages (mesa)
    # needing newer base libraries than the image has ("could not satisfy dependencies").
    pacman -Syu --noconfirm --needed vulkan-icd-loader libx11 libxcb libxext libxfixes libxrender \
        libxi libxcursor libxrandr wayland alsa-lib libpulse systemd-libs fontconfig freetype2 zlib \
        libdrm mesa libglvnd gcc-libs >/dev/null
fi
grep -m1 PRETTY_NAME /etc/os-release
ldd --version | sed -n 1p  # head would end ldd with SIGPIPE (pipefail)
fail() { echo "FAIL: $*" >&2; exit 1; }
unset DISPLAY WAYLAND_DISPLAY
exe="$release/everybodys-golf"
[[ -x $exe ]] || fail "Runner is missing or not executable"
[[ ! -e $release/prepare && ! -e $release/resogun-setup ]] || fail "Legacy setup is in the release"
if ldd "$exe" | grep 'not found'; then fail "Runner libraries are missing"; fi
"$exe" --help | grep -- '--prepare-only' >/dev/null || fail "Launcher help is missing"
"$exe" --version | grep "Everybody's Golf" >/dev/null || fail "Launcher version is missing"

work=$(mktemp -d)
mkdir -p "$work/dump/sce_sys" "$work/dump/sce_module"
: >"$work/dump/eboot.bin"
cd "$work"
status=0
"$exe" --data-dir "$work/portable" --game "$work/dump" --prepare-only >"$work/prepare.log" 2>&1 || status=$?
cat "$work/prepare.log"
[[ $status == 1 ]] || fail "Empty EBOOT returned $status"
grep -q 'empty or oversized input' "$work/prepare.log" || fail "Native preparation did not reject an empty EBOOT"
[[ ! -e $work/portable/paths.ini ]] || fail "Failed preparation wrote paths.ini"
[[ ! -e $work/user && ! -e $work/gpu && ! -e $work/settings.ini ]] || fail "Working directory received state"
echo "PASS: $(grep -m1 PRETTY_NAME /etc/os-release | cut -d= -f2)"
