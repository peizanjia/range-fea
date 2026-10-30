import contextlib
import hashlib
import io
import json
from pathlib import Path
import stat
import tempfile
import types
import unittest
from unittest import mock

import inventory_adapter_v43 as adapter
from inventory_adapter_v43 import InventoryError, verify_install_tree


CHECK_IDS = (
    "installFilesHashed",
    "projectExecutablesAtCmakeInstallPaths",
    "installTreeStableAfterDependencyScan",
    "projectExecutablesMatchReleasePeers",
    "supplierExecutableSourcesMatch",
    "installedExecutableProvenanceComplete",
    "installedExecutablesStableAcrossDumpbin",
    "peDependentsReadable",
    "noLibompFile",
    "noLibompImport",
    "requiredExecutablesImportVcomp140",
    "packagedVcompMatchesSuppliedRedist",
    "qtSupplierNoticeFiles",
    "qtAttributionPages",
    "qtSbomFiles",
    "tetgenLicense",
    "agentOffInstalledPathAbsence",
    "agentOffOperationsDocumentationAbsent",
)


def passing_checks():
    return [{"id": check_id, "status": "PASS", "details": {}} for check_id in CHECK_IDS]


def make_fixture(base, actual_files, report_rows=None, report_directories=None,
                 actual_empty_directories=()):
    install = Path(base) / "install"
    install.mkdir(exist_ok=True)
    rows = []
    actual_paths = {}
    actual_directories = set()

    def add_parent_directories(relative):
        parts = relative.split("/")
        for index in range(1, len(parts)):
            actual_directories.add("/".join(parts[:index]))

    for relative in actual_empty_directories:
        install.joinpath(*relative.split("/")).mkdir(parents=True, exist_ok=True)
        add_parent_directories(relative)
        actual_directories.add(relative)

    for relative, content in actual_files.items():
        path = install.joinpath(*relative.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        add_parent_directories(relative)
        path.write_bytes(content)
        actual_paths[relative] = path
        rows.append({
            "path": relative,
            "sizeBytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        })
    if report_rows is not None:
        rows = report_rows
    if report_directories is None:
        report_directory_set = set(actual_directories)
        for row in rows:
            parts = row["path"].split("/")[:-1]
            for index in range(1, len(parts) + 1):
                report_directory_set.add("/".join(parts[:index]))
        report_directories = sorted(report_directory_set, key=lambda value: (value.casefold(), value))

    report = {
        "schemaVersion": 2,
        "inventoryStatus": "PASS",
        "releaseQualificationStatus": "INCOMPLETE_LINK_MAP_NOT_SUPPLIED",
        "variant": "OFF",
        "installRoot": str(install),
        "fileCount": len(rows),
        "files": rows,
        "directories": list(report_directories),
        "checks": passing_checks(),
    }
    report_path = Path(base) / "inventory.json"
    raw = json.dumps(report, separators=(",", ":")).encode("utf-8")
    report_path.write_bytes(raw)
    return install, report_path, hashlib.sha256(raw).hexdigest(), actual_paths


class FakeEntry:
    def __init__(self, path, details):
        self.path = str(path)
        self.name = Path(path).name
        self.details = details

    def stat(self, follow_symlinks=False):
        return self.details


class FakeScan:
    def __init__(self, entries):
        self.entries = entries

    def __enter__(self):
        return iter(self.entries)

    def __exit__(self, exc_type, exc_value, traceback):
        return False


class InstallTreeReviewV43Tests(unittest.TestCase):
    def test_matching_synthetic_off_tree_reports_dynamic_count_and_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, _ = make_fixture(temp, {
                "bin/fea.exe": b"synthetic-gui",
                "runtime/python/python.exe": b"synthetic-python-runtime",
            })

            result = verify_install_tree(report, str(install), digest)

            self.assertEqual("INVENTORY_TREE_MATCH", result.as_json()["status"])
            self.assertEqual(2, result.file_count)
            self.assertEqual(len(b"synthetic-gui") + len(b"synthetic-python-runtime"), result.total_bytes)
            self.assertFalse(result.as_json()["goPermitted"])
            self.assertFalse(result.as_json()["guiAdmissionEvaluated"])
            self.assertTrue(all(value is False for value in result.as_json()["productActions"].values()))

    def test_reported_empty_directory_is_preserved_and_matches(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, _ = make_fixture(
                temp,
                {"bin/fea.exe": b"gui"},
                actual_empty_directories=("share/range-workflow/examples/__pycache__",),
            )

            result = verify_install_tree(report, str(install), digest)

            self.assertEqual("INVENTORY_TREE_MATCH", result.as_json()["status"])
            self.assertEqual(5, result.as_json()["directoryCount"])
            self.assertIn("share/range-workflow/examples/__pycache__",
                          json.loads(report.read_text(encoding="utf-8"))["directories"])

    def test_added_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            (install / "extra.dll").write_bytes(b"unreported")

            with self.assertRaisesRegex(InventoryError, "file set mismatch.*added"):
                verify_install_tree(report, str(install), digest)

    def test_added_empty_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            (install / "empty-extra").mkdir()

            with self.assertRaisesRegex(InventoryError, "directory set mismatch.*added"):
                verify_install_tree(report, str(install), digest)

    def test_missing_report_parent_directories_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, _, _, _ = make_fixture(temp, {})
            row = {
                "path": "bin/sub/fea.exe",
                "sizeBytes": 3,
                "sha256": hashlib.sha256(b"gui").hexdigest(),
            }
            _, report, digest, _ = make_fixture(temp, {}, [row])
            report_json = json.loads(report.read_text(encoding="utf-8"))
            report_json["installRoot"] = str(install)
            raw = json.dumps(report_json, separators=(",", ":")).encode("utf-8")
            report.write_bytes(raw)
            digest = hashlib.sha256(raw).hexdigest()

            with self.assertRaisesRegex(InventoryError, "directory set mismatch.*missing"):
                verify_install_tree(report, str(install), digest)

    def test_missing_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, paths = make_fixture(temp, {"bin/fea.exe": b"gui"})
            paths["bin/fea.exe"].unlink()

            with self.assertRaisesRegex(InventoryError, "file set mismatch.*missing"):
                verify_install_tree(report, str(install), digest)

    def test_changed_file_size_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, paths = make_fixture(temp, {"bin/fea.exe": b"gui"})
            paths["bin/fea.exe"].write_bytes(b"gui-now-larger")

            with self.assertRaisesRegex(InventoryError, "size differs"):
                verify_install_tree(report, str(install), digest)

    def test_changed_file_hash_with_same_size_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, paths = make_fixture(temp, {"bin/fea.exe": b"gui"})
            paths["bin/fea.exe"].write_bytes(b"bad")

            with self.assertRaisesRegex(InventoryError, "SHA256 differs"):
                verify_install_tree(report, str(install), digest)

    def test_reported_size_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, _, _, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            row = {
                "path": "bin/fea.exe",
                "sizeBytes": len(b"gui") + 1,
                "sha256": hashlib.sha256(b"gui").hexdigest(),
            }
            _, report, digest, _ = make_fixture(temp, {}, [row])
            report_json = json.loads(report.read_text(encoding="utf-8"))
            report_json["installRoot"] = str(install)
            raw = json.dumps(report_json, separators=(",", ":")).encode("utf-8")
            report.write_bytes(raw)
            digest = hashlib.sha256(raw).hexdigest()

            with self.assertRaisesRegex(InventoryError, "size differs"):
                verify_install_tree(report, str(install), digest)

    def test_case_only_path_change_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, _, _, _ = make_fixture(temp, {"Bin/FEA.exe": b"gui"})
            row = {
                "path": "bin/fea.exe",
                "sizeBytes": len(b"gui"),
                "sha256": hashlib.sha256(b"gui").hexdigest(),
            }
            report = Path(temp) / "inventory.json"
            report_json = {
                "schemaVersion": 2,
                "inventoryStatus": "PASS",
                "variant": "OFF",
                "installRoot": str(install),
                "fileCount": 1,
                "files": [row],
                "directories": ["bin"],
                "checks": passing_checks(),
            }
            raw = json.dumps(report_json, separators=(",", ":")).encode("utf-8")
            report.write_bytes(raw)

            with self.assertRaisesRegex(InventoryError, "casing differs"):
                verify_install_tree(report, str(install), hashlib.sha256(raw).hexdigest())

    def test_case_only_parent_directory_change_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, _, _, _ = make_fixture(temp, {"Bin/fea.exe": b"gui"})
            row = {
                "path": "bin/fea.exe",
                "sizeBytes": len(b"gui"),
                "sha256": hashlib.sha256(b"gui").hexdigest(),
            }
            _, report, _, _ = make_fixture(temp, {}, [row])
            report_json = json.loads(report.read_text(encoding="utf-8"))
            report_json["installRoot"] = str(install)
            raw = json.dumps(report_json, separators=(",", ":")).encode("utf-8")
            report.write_bytes(raw)

            with self.assertRaisesRegex(InventoryError, "directory casing differs"):
                verify_install_tree(report, str(install), hashlib.sha256(raw).hexdigest())

    def test_reparse_file_and_directory_entries_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            fake_path = install / "link-entry"
            fixtures = (
                types.SimpleNamespace(st_mode=stat.S_IFLNK, st_file_attributes=0),
                types.SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=adapter.REPARSE_FLAG),
            )
            for details in fixtures:
                with self.subTest(details=details):
                    fake_scan = FakeScan([FakeEntry(fake_path, details)])
                    with mock.patch.object(adapter.os, "scandir", return_value=fake_scan):
                        with self.assertRaisesRegex(InventoryError, "symlink or reparse point"):
                            verify_install_tree(report, str(install), digest)

    def test_case_insensitive_collision_in_current_tree_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, paths = make_fixture(temp, {"A.txt": b"x"})
            original = paths["A.txt"].lstat()
            fake_scan = FakeScan([
                FakeEntry(install / "A.txt", original),
                FakeEntry(install / "a.txt", original),
            ])
            with mock.patch.object(adapter.os, "scandir", return_value=fake_scan):
                with self.assertRaisesRegex(InventoryError, "case-insensitive path collision"):
                    verify_install_tree(report, str(install), digest)

    def test_case_insensitive_directory_collision_in_current_tree_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, _ = make_fixture(temp, {"root.txt": b"x"})
            (install / "A").mkdir()
            original = (install / "A").lstat()
            fake_scan = FakeScan([
                FakeEntry(install / "A", original),
                FakeEntry(install / "a", original),
            ])
            with mock.patch.object(adapter.os, "scandir", return_value=fake_scan):
                with self.assertRaisesRegex(InventoryError, "case-insensitive path collision"):
                    verify_install_tree(report, str(install), digest)

    def test_path_escape_is_rejected_by_inventory_adapter(self):
        with tempfile.TemporaryDirectory() as temp:
            install, _, _, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            with self.assertRaisesRegex(InventoryError, "relative POSIX path|canonical"):
                adapter._validate_relative_path("../outside.exe")
            with self.assertRaisesRegex(InventoryError, "escapes installRoot"):
                adapter._assert_child_within_root(install.parent / "outside.exe", install)

    def test_public_tree_review_requires_a_report_path_and_external_pin(self):
        with tempfile.TemporaryDirectory() as temp:
            install, _, _, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            snapshot = adapter.adapt_inventory_report({
                "schemaVersion": 2,
                "inventoryStatus": "PASS",
                "variant": "OFF",
                "installRoot": str(install),
                "fileCount": 1,
                "files": [{
                    "path": "bin/fea.exe",
                    "sizeBytes": 3,
                    "sha256": hashlib.sha256(b"gui").hexdigest(),
                }],
                "directories": ["bin"],
                "checks": passing_checks(),
            }, str(install))
            forged = adapter.InventorySnapshot(
                install_root=snapshot.install_root,
                variant=snapshot.variant,
                report_sha256="f" * 64,
                files=snapshot.files,
                directories=snapshot.directories,
            )

            self.assertIsNone(snapshot.report_sha256)
            self.assertFalse(hasattr(adapter, "review_install_tree"))
            with self.assertRaises(TypeError):
                verify_install_tree(forged, str(install), "f" * 64)

    def test_inventory_report_with_directory_case_collision_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install, _, _, _ = make_fixture(temp, {})
            rows = [
                {"path": "A/one.bin", "sizeBytes": 1, "sha256": hashlib.sha256(b"1").hexdigest()},
                {"path": "a/two.bin", "sizeBytes": 1, "sha256": hashlib.sha256(b"2").hexdigest()},
            ]
            _, report, _, _ = make_fixture(temp, {}, rows)
            report_json = json.loads(report.read_text(encoding="utf-8"))
            report_json["installRoot"] = str(install)
            raw = json.dumps(report_json, separators=(",", ":")).encode("utf-8")
            report.write_bytes(raw)

            with self.assertRaisesRegex(InventoryError, "duplicate or case-colliding"):
                verify_install_tree(report, str(install), hashlib.sha256(raw).hexdigest())

    def test_cli_returns_bounded_tree_match_result_without_product_admission(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, digest, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                exit_code = adapter.main([
                    "--install-root", str(install),
                    "--inventory-report", str(report),
                    "--inventory-sha256", digest,
                    "--variant", "OFF",
                ])

            self.assertEqual(0, exit_code)
            self.assertEqual("", stderr.getvalue())
            result = json.loads(stdout.getvalue())
            self.assertEqual("INVENTORY_TREE_MATCH", result["status"])
            self.assertEqual(1, result["fileCount"])
            self.assertFalse(result["goPermitted"])
            self.assertFalse(result["guiAdmissionEvaluated"])

    def test_cli_rejects_wrong_external_report_pin(self):
        with tempfile.TemporaryDirectory() as temp:
            install, report, _, _ = make_fixture(temp, {"bin/fea.exe": b"gui"})
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                exit_code = adapter.main([
                    "--install-root", str(install),
                    "--inventory-report", str(report),
                    "--inventory-sha256", "0" * 64,
                    "--variant", "OFF",
                ])

            self.assertEqual(2, exit_code)
            self.assertEqual("", stdout.getvalue())
            result = json.loads(stderr.getvalue())
            self.assertEqual("INVENTORY_TREE_MISMATCH", result["status"])
            self.assertFalse(result["goPermitted"])
            self.assertTrue(all(value is False for value in result["productActions"].values()))


if __name__ == "__main__":
    unittest.main()
