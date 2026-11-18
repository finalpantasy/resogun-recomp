"""Linux dependency listing used by the release packager."""
from pathlib import Path
from subprocess import CompletedProcess
import sys
import unittest
from unittest.mock import patch

from paths import ROOT

sys.path.insert(0, str(ROOT / "tools"))
from package import so_closure


class LinuxLibraryTests(unittest.TestCase):
    def test_preserves_loader_name_and_resolved_path(self):
        listing = (
            "\tlibSDL3.so.0 => /opt/Golf SDK/lib/libSDL3.so.0.4.16 (0x1000)\n"
            "\tlibvulkan.so.1 => /lib/libvulkan.so.1 (0x2000)\n"
            "\tlibfmt.so.10 => /lib/libfmt.so.10 (0x3000)\n"
            "\t/lib64/ld-linux-x86-64.so.2 (0x4000)\n"
        )
        process = CompletedProcess(["ldd"], 0, stdout=listing)
        with patch("package.subprocess.run", return_value=process):
            self.assertEqual(so_closure([Path("everybodys-golf")]), {
                "libSDL3.so.0": Path("/opt/Golf SDK/lib/libSDL3.so.0.4.16"),
                "libfmt.so.10": Path("/lib/libfmt.so.10"),
            })

    def test_reports_unresolved_bundled_dependency(self):
        process = CompletedProcess(["ldd"], 0, stdout="\tlibSDL3.so.0 => not found\n")
        with patch("package.subprocess.run", return_value=process):
            with self.assertRaisesRegex(SystemExit, r"ldd cannot find libSDL3\.so\.0"):
                so_closure([Path("everybodys-golf")])
