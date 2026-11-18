from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT))
import run


class RunIsaTests(unittest.TestCase):
    def test_rewrite_flag_reaches_native_launcher(self):
        _, args = run.command_line(["--isa-rewrite-mode", "0"], {})
        self.assertEqual(args, ["--isa-rewrite-mode", "0"])


if __name__ == "__main__":
    unittest.main()
