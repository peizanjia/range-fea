"""Real native preparation and immutable-source failure checks."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

NATIVE, SOURCE = map(Path, sys.argv[1:3])
sys.argv[1:3] = []


class NativeTests(unittest.TestCase):
    def run_native(self, *args, success=True):
        result = subprocess.run([str(NATIVE), *map(str, args)], capture_output=True,
                                text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stderr + result.stdout)
        return json.loads(result.stdout)

    def test_prepare_and_reject_overwrite(self):
        original = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            passive = Path(directory) / "passive.rbm"
            active = Path(directory) / "active.ram"
            self.run_native("prepare", "--input", SOURCE, "--output", passive,
                            "--steps", 12, "--dt", .005, "--passive")
            prepared = self.run_native("inspect", "--input", passive)
            self.assertFalse(prepared["controlEnabled"])
            self.assertFalse(prepared["checkpointAvailable"])
            self.assertEqual(prepared["plannedSteps"], 12)
            self.assertEqual(prepared["configuredDt"], .005)
            before = passive.read_bytes()
            self.run_native("prepare", "--input", SOURCE, "--output", passive,
                            "--steps", 20, "--dt", .005, success=False)
            self.assertEqual(passive.read_bytes(), before)
            self.run_native("prepare", "--input", SOURCE, "--output", active,
                            "--steps", 20, "--dt", .005)
            self.assertTrue(self.run_native("inspect", "--input", active)["controlEnabled"])
        self.assertEqual(hashlib.sha256(SOURCE.read_bytes()).hexdigest(), original)

    def test_invalid_config_never_publishes(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "invalid.ram"
            for steps, dt, extra in [(0, .005, []), (12, "nan", []), (12, .005, ["--resume"])]:
                self.run_native("prepare", "--input", SOURCE, "--output", output,
                                "--steps", steps, "--dt", dt, *extra, success=False)
                self.assertFalse(output.exists())

    def test_native_link_binds_target_and_rejects_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "complete.ram"
            target.write_bytes(SOURCE.read_bytes())
            link = Path(directory) / "latest.ram"
            link.write_text('1.3.17\n5\n"complete.ram"\n', encoding="utf-8")
            response = self.run_native("inspect", "--input", link)
            self.assertEqual(response["sourceSha256"], hashlib.sha256(target.read_bytes()).hexdigest())
            self.assertEqual(Path(response["sourcePath"]), target)
            self.assertEqual(len(response["sourceFiles"]), 2)
            link.write_text('1.3.17\n5\n"latest.ram"\n', encoding="utf-8")
            self.run_native("inspect", "--input", link, success=False)


if __name__ == "__main__":
    unittest.main()
