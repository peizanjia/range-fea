"""Fast integrity checks for the runtime assembler; numeric import smoke is in build()."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("private_runtime_builder", Path(__file__).with_name("build_private_python.py"))
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class RuntimeAssemblyIntegrity(unittest.TestCase):
    def test_archive_cannot_escape_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "bad.zip"
            for name in ("../outside", "/absolute", "C:/absolute", "a\\..\\outside"):
                with self.subTest(name=name):
                    with zipfile.ZipFile(archive, "w") as bundle:
                        bundle.writestr(name, b"unsafe")
                    with self.assertRaises(ValueError):
                        builder.extract(archive, root / "output")
            self.assertFalse((root / "outside").exists())

    @unittest.skipUnless(builder.sys.platform == "win32", "Windows runtime assembler")
    def test_tampered_cache_is_rejected_before_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "archive.zip").write_bytes(b"tampered")
            lock = {"schemaVersion": 1, "platform": "win_amd64", "files": [
                {"filename": "archive.zip", "sha256": "0" * 64}]}
            (root / "lock.json").write_text(json.dumps(lock), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                builder.build(root / "lock.json", root, root / "output", True)
            self.assertFalse((root / "output").exists())

    @unittest.skipUnless(builder.sys.platform == "win32", "Windows runtime assembler")
    def test_existing_runtime_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "existing"
            output.mkdir()
            sentinel = output / "preserve.txt"
            sentinel.write_text("preserved", encoding="utf-8")
            lock = {"schemaVersion": 1, "platform": "win_amd64", "files": []}
            (root / "lock.json").write_text(json.dumps(lock), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "already exists"):
                builder.build(root / "lock.json", root, output, True)
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserved")


if __name__ == "__main__":
    unittest.main()
