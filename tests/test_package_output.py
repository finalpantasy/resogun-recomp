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
    def test_failed_symbols_write_preserves_both_archives(self):
        fixtures = (
            (False, "everybodys-golf.exe", "fixture", ".zip"),
            (True, "everybodys-golf", "fixture-linux", ".tar.gz"),
        )
        for linux, executable, name, suffix in fixtures:
            with self.subTest(suffix=suffix), TemporaryDirectory() as temp:
                root = Path(temp)
                output = root / "out"
                dist = root / "dist"
                output.mkdir()
                dist.mkdir()
                (dist / executable).write_bytes(b"player executable")
                (output / executable).write_bytes(b"executable with symbols")
                player = output / f"{name}{suffix}"
                symbols = output / f"{name}-symbols{suffix}"
                player.write_bytes(b"prior player archive")
                symbols.write_bytes(b"prior symbols archive")
                write_archive = package.write_archive

                def fail_symbols(path, entries):
                    if path.name == symbols.name:
                        path.write_bytes(b"partial symbols archive")
                        raise OSError("simulated write failure")
                    return write_archive(path, entries)

                with (
                    patch.object(package, "PORT", root),
                    patch.object(package, "LINUX", linux),
                    patch.object(package, "EXES", (executable,)),
                    patch.object(package, "write_archive", side_effect=fail_symbols),
                ):
                    with self.assertRaisesRegex(OSError, "simulated write failure"):
                        package.archive_bundle(dist, [executable], "fixture", debug=False, include_appimage=False)

                self.assertEqual(player.read_bytes(), b"prior player archive")
                self.assertEqual(symbols.read_bytes(), b"prior symbols archive")
                self.assertEqual(set(output.iterdir()), {output / executable, player, symbols})

    def test_failed_appimage_keeps_previous_tarballs_and_image(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "out"
            dist = root / "dist"
            output.mkdir()
            dist.mkdir()
            executable = "everybodys-golf"
            (dist / executable).write_bytes(b"player executable")
            (output / executable).write_bytes(b"executable with symbols")
            previous = (
                output / "fixture-linux.tar.gz",
                output / "fixture-linux-symbols.tar.gz",
                output / "fixture-linux-x86_64.AppImage",
            )
            for path in previous:
                path.write_bytes(path.name.encode())

            def fail_appimage(_out, _shipped, release, tarball):
                self.assertNotEqual(tarball.parent, output)
                self.assertTrue(tarball.is_file())
                (tarball.parent / f"{release}-linux-x86_64.AppImage").write_bytes(b"partial image")
                raise SystemExit("simulated AppImage failure")

            with (
                patch.object(package, "PORT", root),
                patch.object(package, "LINUX", True),
                patch.object(package, "EXES", (executable,)),
                patch.object(package, "appimage", side_effect=fail_appimage),
            ):
                with self.assertRaisesRegex(SystemExit, "simulated AppImage failure"):
                    package.archive_bundle(dist, [executable], "fixture", debug=False, include_appimage=True)

            for path in previous:
                self.assertEqual(path.read_bytes(), path.name.encode())
            self.assertEqual(set(output.iterdir()), {output / executable, *previous})

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
