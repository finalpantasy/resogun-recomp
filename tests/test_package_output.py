"""Guard the packager against pruning files from a user-supplied output folder."""
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from paths import ROOT

sys.path.insert(0, str(ROOT / "tools"))
import package


class PackageOutputTests(unittest.TestCase):
    def test_dependency_conflicts_leave_destination_untouched(self):
        fixtures = (
            (False, "everybodys-golf.exe", "loose.dll", "contains loose DLLs"),
            (False, "everybodys-golf.exe", "libs/local.dll", "outside this package"),
            (True, "everybodys-golf", "lib/local.so", "outside this package"),
        )
        for linux, executable, conflict, message in fixtures:
            with self.subTest(conflict=conflict), TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "out").mkdir()
                (root / "out" / executable).write_bytes(b"build")
                destination = root / "custom destination"
                existing = destination / conflict
                existing.parent.mkdir(parents=True)
                existing.write_bytes(b"user file")

                with (
                    patch.object(package, "PORT", root),
                    patch.object(package, "LINUX", linux),
                    patch.object(package, "EXES", (executable,)),
                    patch.object(package, "git", return_value="fixture"),
                    patch.object(package, "verify_build"),
                    patch.object(package, "dll_closure", return_value=[]),
                    patch.object(package, "so_closure", return_value={}),
                    patch.object(sys, "argv", ["package.py", "--out", str(destination)]),
                ):
                    with self.assertRaisesRegex(SystemExit, message):
                        package.main()

                self.assertEqual(existing.read_bytes(), b"user file")
                self.assertFalse((destination / executable).exists())
