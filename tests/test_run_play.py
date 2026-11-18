from pathlib import Path
from tempfile import TemporaryDirectory
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT))
import run


class RunPlayTests(unittest.TestCase):
    def test_forwards_launch_args(self):
        exe, args = run.command_line(["play", "--game", "dump", "--user", "save"], {"EG_EXE": "explicit/runner.exe"})
        self.assertEqual(exe, Path("explicit/runner.exe"))
        self.assertEqual(args, ["--game", "dump", "--user", "save"])

    def test_prepare_uses_portable_root(self):
        _, args = run.command_line(["prepare", "--game", "dump", "--out", "test build"], {})
        self.assertEqual(args, ["--game", "dump", "--data-dir", "test build", "--prepare-only", "--reprepare"])

    def test_check_can_use_existing_developer_image(self):
        with TemporaryDirectory() as folder:
            (Path(folder) / "boot-linked.bin").touch()
            _, args = run.command_line(["check", "--game", "dump", "--out", folder], {})
        self.assertEqual(args, ["--game", "dump", "--prepared", folder, "--check-imports"])


if __name__ == "__main__":
    unittest.main()
