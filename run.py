#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Developer convenience wrapper for the native Everybody's Golf launcher."""
import os
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parent
WINDOWS = sys.platform == "win32"


def command_line(argv, env):
    suffix = ".exe" if WINDOWS else ""
    override = env.get("EG_EXE") or env.get("RESOGUN_EXE")
    exe = Path(override) if override else PORT / "out" / ("everybodys-golf" + suffix)
    args = list(argv)
    mode = args.pop(0) if args and args[0] in {"play", "check", "prepare"} else "play"
    if "--out" in args:
        i = args.index("--out")
        if i + 1 >= len(args):
            raise ValueError("--out requires a directory")
        directory = Path(args[i + 1])
        args[i] = "--prepared" if mode != "prepare" and (directory / "boot-linked.bin").is_file() else "--data-dir"
    if mode == "prepare":
        args.extend(("--prepare-only", "--reprepare"))
    elif mode == "check":
        args.append("--check-imports")
    if not any(flag in args for flag in ("--timeout", "--prepare-only", "--check-imports")) and env.get("RESOGUN_TIMEOUT"):
        args.extend(("--timeout", env["RESOGUN_TIMEOUT"]))
    return exe, args


def main():
    env = dict(os.environ)
    try:
        exe, args = command_line(sys.argv[1:], env)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    if not exe.is_file():
        print(f"{exe} not found. Build it with bash build.sh.", file=sys.stderr)
        return 1
    if WINDOWS and not (exe.parent / "libs" / "SDL3.dll").is_file():
        roots = [Path(env["MSYS2_ROOT"])] if env.get("MSYS2_ROOT") else []
        roots += [PORT.parent.parent / "tools/msys2/msys64", Path(r"C:\msys64")]
        bins = (root / "clang64" / "bin" for root in roots)
        clang64 = next((directory for directory in bins if (directory / "SDL3.dll").is_file()), None)
        if clang64 is None:
            print("SDL3.dll not found. Set MSYS2_ROOT to the MSYS2 install used to build the launcher.", file=sys.stderr)
            return 1
        env["PATH"] = f"{clang64}{os.pathsep}{env.get('PATH', '')}"
    return subprocess.run([str(exe), *args], env=env, stdin=subprocess.DEVNULL).returncode


if __name__ == "__main__":
    sys.exit(main())
