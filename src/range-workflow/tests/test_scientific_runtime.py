"""Runtime identity compatibility and complete cached/forced verification."""
import json
import importlib.util
import marshal
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import extensions, scientific_extensions, scientific_runtime
from range_workflow.service import WorkflowError, digest, file_hash


class ScientificRuntime(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.python = self.root / "python.exe"
        self.library = self.root / "Lib" / "nested" / "library.dll"
        self.library.parent.mkdir(parents=True)
        self.python.write_bytes(b"test interpreter")
        self.library.write_bytes(b"trusted library")
        self.lock = self.root / "runtime-lock.json"
        self.lock.write_text(json.dumps({"schemaVersion": 1,
            "runtimeId": scientific_runtime.EXPECTED_RUNTIME_ID, "platform": "win_amd64",
            "files": [{"name": name, "version": version,
                       "sha256": scientific_runtime.EXPECTED_ARCHIVES[name]}
                      for name, version in scientific_runtime.EXPECTED_PACKAGES.items()]}), encoding="utf-8")
        self.files = {path.relative_to(self.root).as_posix(): file_hash(path)
                      for path in (self.python, self.library, self.lock)}
        self.manifest = self.root / "runtime-files.json"
        self.manifest.write_text(json.dumps(self.files), encoding="utf-8")
        self.service = SimpleNamespace(python=self.python)
        extensions._RUNTIME_VERIFICATION_CACHE.clear()
        self.addCleanup(extensions._RUNTIME_VERIFICATION_CACHE.clear)

    def assert_changed(self, function):
        with self.assertRaises(WorkflowError) as caught:
            function()
        self.assertEqual(caught.exception.code, "RUNTIME_CHANGED")

    def test_v1_identity_keeps_original_fields_and_hash(self):
        identity = {"protocolVersion": "range.extension/v1", "pythonPath": str(self.python.resolve()),
            "pythonSha256": file_hash(self.python), "pythonVersion": sys.version.split()[0],
            "runtimeLockSha256": file_hash(self.lock), "runtimeFilesSha256": file_hash(self.manifest),
            "extensionChildSha256": file_hash(Path(extensions.__file__).with_name("extension_child.py")),
            "profile": "packaged", "verifiedFiles": True, "dependencies": [], "flags": ["-I", "-S"]}
        self.assertEqual(extensions.runtime_identity(self.service),
                         {"identity": identity, "runtimeHash": digest(identity), "profile": "packaged"})

    def test_v2_identity_keeps_hash_without_reading_v1_child(self):
        legacy = extensions.runtime_identity(self.service)
        expected = dict(legacy["identity"])
        harness = {name: file_hash(Path(scientific_runtime.__file__).with_name(name)) for name in
                   ("extension_child_v2.py", "scientific_contract.py", "scientific_reference.py", "scientific_oracle_child.py")}
        expected.update(protocolVersion="range.extension/v2", pythonVersion="3.13.5",
            extensionChildSha256=harness["extension_child_v2.py"], scientificHarnessSha256=harness,
            runtimeId=scientific_runtime.EXPECTED_RUNTIME_ID,
            dependencies=[{"name": name, "version": scientific_runtime.EXPECTED_PACKAGES[name]}
                          for name in ("numpy", "scipy")],
            versionEvidence="verified-frozen-runtime-lock", flags=["-I", "-B"])
        def checked_hash(path):
            self.assertNotEqual(Path(path).name, "extension_child.py")
            return file_hash(path)
        with mock.patch.object(extensions, "file_hash", side_effect=checked_hash), \
                mock.patch.object(scientific_runtime, "file_hash", side_effect=checked_hash):
            actual = scientific_runtime.runtime_identity(self.service)
        self.assertEqual(actual, {"identity": expected, "runtimeHash": digest(expected), "profile": "packaged"})

    def test_cached_identity_still_inspects_complete_file_set_and_stats(self):
        scientific_runtime.runtime_identity(self.service, force_verify=True)
        with mock.patch.object(extensions, "_runtime_manifest_snapshot", wraps=extensions._runtime_manifest_snapshot) as snapshots, \
                mock.patch.object(extensions, "file_hash", wraps=file_hash) as hashes:
            scientific_runtime.runtime_identity(self.service)
        self.assertEqual(snapshots.call_count, 1)
        self.assertNotIn(self.library, [call.args[0] for call in hashes.call_args_list])
        self.library.write_bytes(b"changed library")
        with self.assertRaises(WorkflowError) as caught:
            scientific_runtime.runtime_identity(self.service)
        self.assertEqual(caught.exception.code, "RUNTIME_VERIFICATION_REQUIRED")

    def test_snapshot_preserves_complete_os_stat_tokens(self):
        expected = tuple((relative, *extensions._runtime_stat_token(self.root / relative))
                         for relative in sorted(self.files))
        self.assertEqual(extensions._runtime_manifest_snapshot(self.root, self.files), expected)

    def test_removed_file_is_rejected_even_with_warm_cache(self):
        scientific_runtime.runtime_identity(self.service, force_verify=True)
        self.library.unlink()
        self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service))

    def test_added_file_is_rejected_even_with_warm_cache(self):
        scientific_runtime.runtime_identity(self.service, force_verify=True)
        (self.library.parent / "injected.dll").write_bytes(b"unlisted")
        self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service))

    def test_stamp_is_cheap_and_changes_with_identity_inputs(self):
        with mock.patch.object(extensions, "_runtime_manifest_snapshot", side_effect=AssertionError("tree scan")):
            stamp = scientific_runtime.environment_stamp(self.service)
            self.assertEqual(stamp, scientific_runtime.environment_stamp(self.service))
            self.library.write_bytes(b"changed library")
            # A stamp is an input pin, never proof that DLLs were verified.
            self.assertEqual(stamp, scientific_runtime.environment_stamp(self.service))
            self.manifest.write_bytes(self.manifest.read_bytes() + b" ")
            self.assertNotEqual(stamp, scientific_runtime.environment_stamp(self.service))

    def test_stamp_rejects_changed_frozen_version(self):
        lock = json.loads(self.lock.read_bytes())
        lock["files"][0]["version"] = "3.13.6"
        self.lock.write_text(json.dumps(lock), encoding="utf-8")
        self.assert_changed(lambda: scientific_runtime.environment_stamp(self.service))

    def test_stamp_bounds_metadata_before_read(self):
        self.manifest.write_bytes(b"x" * (extensions.MAX_RUNTIME_MANIFEST_BYTES + 1))
        self.assert_changed(lambda: scientific_runtime.environment_stamp(self.service))

    def test_warm_identity_and_stamp_rehash_scientific_harness(self):
        original = scientific_runtime.runtime_identity(self.service, force_verify=True)
        stamp = scientific_runtime.environment_stamp(self.service)
        def changed_harness(path):
            return "f" * 64 if Path(path).name == "scientific_oracle_child.py" else file_hash(path)
        with mock.patch.object(scientific_runtime, "file_hash", side_effect=changed_harness):
            self.assertNotEqual(original["runtimeHash"], scientific_runtime.runtime_identity(self.service)["runtimeHash"])
            self.assertNotEqual(stamp, scientific_runtime.environment_stamp(self.service))

    def test_force_verification_hashes_all_bytes_even_with_unchanged_snapshot(self):
        scientific_runtime.runtime_identity(self.service, force_verify=True)
        snapshot = extensions._runtime_manifest_snapshot(self.root, self.files)
        before = self.library.stat()
        self.library.write_bytes(b"changed library")
        os.utime(self.library, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertEqual(self.library.stat().st_size, before.st_size)
        with mock.patch.object(extensions, "_runtime_manifest_snapshot", return_value=snapshot):
            self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service, force_verify=True))

    def test_changes_during_byte_verification_are_rejected(self):
        def altered_hash(path):
            value = file_hash(path)
            if Path(path) == self.library:
                self.library.write_bytes(b"changed library")
            return value
        with mock.patch.object(extensions, "file_hash", side_effect=altered_hash):
            self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service, force_verify=True))

    def test_reparse_directory_is_rejected_before_descending(self):
        fake = SimpleNamespace(name="junction", path=str(self.root / "junction"))
        fake.stat = mock.Mock(return_value=SimpleNamespace(st_mode=stat.S_IFDIR,
            st_file_attributes=getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)))
        with mock.patch.object(extensions.os, "scandir") as scan:
            scan.return_value.__enter__.return_value = [fake]
            self.assert_changed(lambda: extensions._runtime_manifest_snapshot(self.root, self.files))
        self.assertEqual(scan.call_count, 1)
        fake.stat.assert_called_once_with(follow_symlinks=False)

    def test_v2_cold_cache_miss_does_not_hash_runtime_libraries(self):
        with mock.patch.object(extensions, "file_hash", wraps=file_hash) as hashes:
            with self.assertRaises(WorkflowError) as caught:
                scientific_runtime.runtime_identity(self.service)
        self.assertEqual(caught.exception.code, "RUNTIME_VERIFICATION_REQUIRED")
        self.assertNotIn(self.library, [call.args[0] for call in hashes.call_args_list])
        self.assertFalse(extensions._RUNTIME_VERIFICATION_CACHE)

    def test_v2_rejects_timestamp_valid_tampered_legacy_bytecode_cache(self):
        source = self.library.parent / "module.py"
        source.write_bytes(b"VALUE = 'trusted'\n")
        info = source.stat()
        cache = self.library.parent / "__pycache__"
        cache.mkdir()
        header = importlib.util.MAGIC_NUMBER + struct.pack("<III", 0, int(info.st_mtime), info.st_size)
        tampered = marshal.dumps(compile("VALUE = 'untrusted'\n", str(source), "exec"))
        (cache / ("module." + sys.implementation.cache_tag + ".pyc")).write_bytes(header + tampered)
        self.files[source.relative_to(self.root).as_posix()] = file_hash(source)
        self.manifest.write_text(json.dumps(self.files), encoding="utf-8")
        legacy = extensions.runtime_identity(self.service)
        self.assertEqual(legacy["profile"], "packaged")
        self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service))
        self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service, force_verify=True))

    def test_v2_rejects_empty_bytecode_directory(self):
        (self.library.parent / "__pycache__").mkdir()
        self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service, force_verify=True))

    def test_v2_rejects_standalone_listed_bytecode(self):
        bytecode = self.library.parent / "injected.pyc"
        bytecode.write_bytes(b"unverified bytecode")
        files = {**self.files, bytecode.relative_to(self.root).as_posix(): file_hash(bytecode)}
        self.manifest.write_text(json.dumps(files), encoding="utf-8")
        self.assertEqual(extensions.runtime_identity(self.service)["profile"], "packaged")
        self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service, force_verify=True))

    def test_cache_hit_and_stamp_do_not_wait_for_forced_byte_io(self):
        original = scientific_runtime.runtime_identity(self.service, force_verify=True)
        entered, release, read_done = threading.Event(), threading.Event(), threading.Event()
        errors, read_results = [], []
        def blocked_hash(path):
            value = file_hash(path)
            if Path(path) == self.library and threading.current_thread() is worker:
                entered.set()
                if not release.wait(5):
                    raise AssertionError("byte verifier barrier timed out")
            return value
        def force():
            try:
                scientific_runtime.runtime_identity(self.service, force_verify=True)
            except BaseException as error:
                errors.append(error)
        def read():
            try:
                read_results.append(scientific_runtime.runtime_identity(self.service))
                read_results.append(scientific_runtime.environment_stamp(self.service))
            except BaseException as error:
                errors.append(error)
            finally:
                read_done.set()
        worker, reader = threading.Thread(target=force), threading.Thread(target=read)
        with mock.patch.object(extensions, "file_hash", side_effect=blocked_hash):
            worker.start()
            try:
                self.assertTrue(entered.wait(3))
                reader.start()
                self.assertTrue(read_done.wait(1), "cache lookup waited for forced byte IO")
            finally:
                release.set()
                worker.join(3)
                if reader.ident is not None:
                    reader.join(3)
        self.assertFalse(worker.is_alive())
        self.assertFalse(reader.is_alive())
        self.assertFalse(errors, errors)
        self.assertEqual(read_results[0], original)
        self.assertIsInstance(read_results[1], str)

    def test_corruption_during_blocked_force_does_not_publish_cache(self):
        entered, release = threading.Event(), threading.Event()
        errors = []
        def blocked_hash(path):
            value = file_hash(path)
            if Path(path) == self.library:
                entered.set()
                if not release.wait(5):
                    raise AssertionError("byte verifier barrier timed out")
            return value
        def force():
            try:
                scientific_runtime.runtime_identity(self.service, force_verify=True)
            except BaseException as error:
                errors.append(error)
        worker = threading.Thread(target=force)
        with mock.patch.object(extensions, "file_hash", side_effect=blocked_hash):
            worker.start()
            try:
                self.assertTrue(entered.wait(3))
                self.library.write_bytes(b"changed library")
            finally:
                release.set()
                worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(len(errors), 1, errors)
        self.assertEqual(errors[0].code, "RUNTIME_CHANGED")
        self.assertFalse(extensions._RUNTIME_VERIFICATION_CACHE)

    def test_manifest_changes_during_force_do_not_publish_cache(self):
        def changed_hash(path):
            value = file_hash(path)
            if Path(path) == self.library:
                self.manifest.write_bytes(self.manifest.read_bytes() + b" ")
            return value
        with mock.patch.object(extensions, "file_hash", side_effect=changed_hash):
            self.assert_changed(lambda: scientific_runtime.runtime_identity(self.service, force_verify=True))
        self.assertFalse(extensions._RUNTIME_VERIFICATION_CACHE)

    def test_generated_child_has_forced_precheck_and_complete_postcheck(self):
        directory = self.root / "job"
        directory.mkdir()
        source = self.root / "candidate.py"
        source.write_text("def process(data): return data\n", encoding="utf-8")
        record = {"runtimeHash": "d" * 64, "sourcePath": "candidate.py", "sourceSha256": file_hash(source)}
        def child(*args, **kwargs):
            (directory / "scientific-result.json").write_bytes(b'{"results":[]}')
        service = SimpleNamespace(python=self.python, trusted_extensions=True,
            extension_timeout_seconds=15, runner=SimpleNamespace(run=mock.Mock(side_effect=child)))
        runtime = {"runtimeHash": "d" * 64}
        with mock.patch.object(scientific_extensions, "runtime_identity", return_value=runtime) as verify, \
                mock.patch.object(extensions, "_safe_relative", return_value=source):
            result, _ = scientific_extensions.execute_source(service, directory, record, {}, None,
                                                               tests={"cases": []})
        self.assertEqual(result, {"results": []})
        self.assertEqual(verify.call_args_list, [mock.call(service, force_verify=True), mock.call(service)])
        self.assertEqual(service.runner.run.call_args.args[0][1:3], ["-I", "-B"])

    def test_validation_forces_full_verification_after_complete_oracle_batch(self):
        directory = self.root / "job"
        directory.mkdir()
        result_path = directory / "scientific-result.json"
        result_path.write_bytes(b'{"results":[]}')
        runtime = {"runtimeHash": "d" * 64}
        record = {"runtimeHash": runtime["runtimeHash"], "candidateId": "candidate",
                  "bundleHash": "e" * 64, "extensionId": "spectral-test"}
        job = {"parameters": {"runtime": runtime, "registryRevision": 0}}
        service = SimpleNamespace(workspace=self.root)
        with mock.patch.object(scientific_extensions, "golden_cases", return_value=[]), \
                mock.patch.object(scientific_extensions, "execute_source", return_value=({"results": []}, file_hash(result_path))), \
                mock.patch.object(scientific_extensions, "private_oracle_batch", return_value=([], "a" * 64)), \
                mock.patch.object(scientific_extensions, "runtime_identity", return_value=runtime) as verify:
            result = scientific_extensions.execute_validate(service, job, directory, None, record,
                                                             {"version": "1.0.0"}, {"cases": []})
        self.assertEqual(result["state"], "validated")
        verify.assert_called_once_with(service, force_verify=True)


if __name__ == "__main__":
    unittest.main()
