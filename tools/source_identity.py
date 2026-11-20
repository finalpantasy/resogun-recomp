#!/usr/bin/env python3
"""Fingerprint the checkout inputs recorded in the runner's build receipt."""

import hashlib
import os
from pathlib import Path
import subprocess
import sys


def source_identity(root: Path) -> str:
    digest = hashlib.sha256()

    def add(data: bytes) -> None:
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)

    def visit(repo: Path, untracked_paths: tuple[str, ...]) -> None:
        def git(*args: str) -> bytes:
            return subprocess.check_output(["git", *args], cwd=repo)

        add(git("rev-parse", "--verify", "HEAD").strip())
        add(git("diff", "--no-ext-diff", "--no-textconv", "--no-color", "--no-renames", "--binary",
                "--submodule=short", "--ignore-submodules=dirty", "HEAD", "--"))
        files = git("ls-files", "--others", "--exclude-standard", "-z", "--", *untracked_paths)
        for name in sorted(files.split(b"\0")[:-1]):
            path = repo / os.fsdecode(name)
            add(name)
            if path.is_symlink():
                add(b"link")
                add(os.fsencode(os.readlink(path)))
            else:
                add(b"file")
                content = hashlib.sha256()
                with path.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        content.update(chunk)
                add(content.digest())
        for entry in git("ls-files", "--stage", "-z").split(b"\0"):
            if not entry.startswith(b"160000 "):
                continue
            name = entry.split(b"\t", 1)[1]
            submodule = repo / os.fsdecode(name)
            add(name)
            if (submodule / ".git").exists():
                visit(submodule, ())
            else:
                add(b"uninitialized")

    # CMake globs can pick up untracked source files. Package outputs live outside these inputs.
    visit(root, ("src", "gpu", "prepare", "packaging", "tools", "res", "prebuilt"))
    return digest.hexdigest()


if __name__ == "__main__":
    print(source_identity(Path(sys.argv[1])))
