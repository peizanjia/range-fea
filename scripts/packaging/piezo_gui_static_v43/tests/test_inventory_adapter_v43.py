import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from inventory_adapter_v43 import (
    MAX_TREE_ENTRIES,
    InventoryError,
    adapt_inventory_report,
    load_inventory_v43,
)


INSTALL_ROOT = r"D:\synthetic\range-fea-v43-off"
REPORT_OUTSIDE_ROOT = r"D:\synthetic\range-fea-v43-audit\inventory.json"
FILE_SHA_A = "a" * 64
FILE_SHA_B = "b" * 64
DIRECTORIES = (
    "bin",
    "runtime",
    "runtime/python",
    "share",
    "share/range-workflow",
    "share/range-workflow/examples",
    "share/range-workflow/examples/__pycache__",
)
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
    return [{"id": check_id, "status": "PASS"} for check_id in CHECK_IDS]


def valid_report():
    rows = [
        {"path": "bin/fea.exe", "sizeBytes": 13, "sha256": FILE_SHA_A},
        {"path": "runtime/python/python.exe", "sizeBytes": 21, "sha256": FILE_SHA_B},
    ]
    return {
        "schemaVersion": 2,
        "inventoryStatus": "PASS",
        "releaseQualificationStatus": "INCOMPLETE_LINK_MAP_NOT_SUPPLIED",
        "variant": "OFF",
        "installRoot": INSTALL_ROOT,
        "fileCount": len(rows),
        "files": rows,
        "directories": list(DIRECTORIES),
        "checks": passing_checks(),
    }


class InventoryAdapterV43Tests(unittest.TestCase):
    def test_schema_v2_off_report_uses_dynamic_row_count_and_size_bytes(self):
        report = valid_report()
        snapshot = adapt_inventory_report(report, INSTALL_ROOT)

        self.assertEqual("OFF", snapshot.variant)
        self.assertEqual(2, snapshot.file_count)
        self.assertEqual({
            "bin/fea.exe": (13, FILE_SHA_A),
            "runtime/python/python.exe": (21, FILE_SHA_B),
        }, snapshot.by_path())
        self.assertEqual(DIRECTORIES, snapshot.directories)
        self.assertIn("share/range-workflow/examples/__pycache__", snapshot.directories)

    def test_dynamic_count_accepts_a_different_synthetic_tree_size(self):
        report = valid_report()
        report["files"] = report["files"][:1]
        report["fileCount"] = len(report["files"])

        snapshot = adapt_inventory_report(report, INSTALL_ROOT)

        self.assertEqual(1, snapshot.file_count)

    def test_inventory_v1_shape_is_rejected(self):
        report = valid_report()
        report.pop("installRoot")
        report["prefix"] = INSTALL_ROOT
        report["files"][0]["bytes"] = report["files"][0].pop("sizeBytes")

        with self.assertRaisesRegex(InventoryError, "installRoot"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_schema_version_one_is_rejected(self):
        report = valid_report()
        report["schemaVersion"] = 1

        with self.assertRaisesRegex(InventoryError, "schemaVersion"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_on_variant_is_rejected_even_if_it_matches_the_call_argument(self):
        report = valid_report()
        report["variant"] = "ON"

        with self.assertRaisesRegex(InventoryError, "variant"):
            adapt_inventory_report(report, INSTALL_ROOT, expected_variant="ON")

    def test_install_root_mismatch_is_rejected(self):
        with self.assertRaisesRegex(InventoryError, "installRoot"):
            adapt_inventory_report(valid_report(), r"D:\synthetic\other-install")

    def test_file_count_mismatch_is_rejected(self):
        report = valid_report()
        report["fileCount"] = len(report["files"]) + 1

        with self.assertRaisesRegex(InventoryError, "dynamic files row count"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_missing_size_bytes_does_not_fall_back_to_v1_bytes(self):
        report = valid_report()
        report["files"][0]["bytes"] = report["files"][0].pop("sizeBytes")

        with self.assertRaisesRegex(InventoryError, "sizeBytes"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_size_bytes_rejects_bool_negative_and_string_values(self):
        for value in (True, -1, "13"):
            with self.subTest(value=value):
                report = valid_report()
                report["files"][0]["sizeBytes"] = value
                with self.assertRaisesRegex(InventoryError, "sizeBytes"):
                    adapt_inventory_report(report, INSTALL_ROOT)

    def test_duplicate_and_case_colliding_paths_are_rejected(self):
        for path in ("bin/fea.exe", "BIN/FEA.EXE"):
            with self.subTest(path=path):
                report = valid_report()
                report["files"].append({"path": path, "sizeBytes": 0, "sha256": FILE_SHA_A})
                report["fileCount"] = len(report["files"])
                with self.assertRaisesRegex(InventoryError, "duplicate or case-colliding"):
                    adapt_inventory_report(report, INSTALL_ROOT)

    def test_case_colliding_parent_directories_are_rejected(self):
        report = valid_report()
        report["files"] = [
            {"path": "A/one.bin", "sizeBytes": 1, "sha256": FILE_SHA_A},
            {"path": "a/two.bin", "sizeBytes": 1, "sha256": FILE_SHA_B},
        ]
        report["fileCount"] = len(report["files"])
        report["directories"] = ["A", "a"]

        with self.assertRaisesRegex(InventoryError, "duplicate or case-colliding"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_directories_are_required_bounded_sorted_and_parent_complete(self):
        report = valid_report()
        report.pop("directories")
        with self.assertRaisesRegex(InventoryError, "directories must be a bounded list"):
            adapt_inventory_report(report, INSTALL_ROOT)

        report = valid_report()
        report["directories"] = list(reversed(report["directories"]))
        with self.assertRaisesRegex(InventoryError, "canonical case-insensitive path order"):
            adapt_inventory_report(report, INSTALL_ROOT)

        report = valid_report()
        report["directories"] = ["runtime/python"]
        with self.assertRaisesRegex(InventoryError, "omit a required parent directory"):
            adapt_inventory_report(report, INSTALL_ROOT)

        report = valid_report()
        report["directories"] = ["d{:06d}".format(index)
                                  for index in range(MAX_TREE_ENTRIES + 1)]
        with self.assertRaisesRegex(InventoryError, "directories must be a bounded list"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_directories_reject_missing_file_parents_and_file_directory_collisions(self):
        report = valid_report()
        report["directories"] = ["bin", "runtime"]
        with self.assertRaisesRegex(InventoryError, "omit a file parent directory"):
            adapt_inventory_report(report, INSTALL_ROOT)

        report = valid_report()
        report["files"].append({"path": "share", "sizeBytes": 0, "sha256": FILE_SHA_A})
        report["fileCount"] = len(report["files"])
        with self.assertRaisesRegex(InventoryError, "also listed as a directory"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_checks_require_the_complete_unique_producer_id_set(self):
        cases = (
            ("missing", lambda checks: checks.pop()),
            ("duplicate", lambda checks: checks.__setitem__(
                -1, {"id": checks[0]["id"], "status": "PASS"})),
            ("unexpected", lambda checks: checks.__setitem__(
                -1, {"id": "synthetic-extra", "status": "PASS"})),
        )
        for name, mutate in cases:
            with self.subTest(mutation=name):
                report = valid_report()
                mutate(report["checks"])
                with self.assertRaisesRegex(InventoryError, "required producer ID exactly once"):
                    adapt_inventory_report(report, INSTALL_ROOT)

    def test_status_only_synthetic_check_is_rejected(self):
        report = valid_report()
        report["checks"] = [{"status": "PASS"}]

        with self.assertRaisesRegex(InventoryError, "check id must be a non-empty string"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_non_pass_required_producer_check_is_rejected(self):
        report = valid_report()
        report["checks"][0]["status"] = "FAIL"

        with self.assertRaisesRegex(InventoryError, "non-PASS check: installFilesHashed"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_noncanonical_or_escaping_paths_are_rejected(self):
        for path in ("../outside.dll", "/bin/fea.exe", "bin\\fea.exe", "bin//fea.exe"):
            with self.subTest(path=path):
                report = valid_report()
                report["files"][0]["path"] = path
                with self.assertRaises(InventoryError):
                    adapt_inventory_report(report, INSTALL_ROOT)

    def test_failed_inventory_or_failed_check_is_rejected(self):
        report = valid_report()
        report["inventoryStatus"] = "FAIL"
        with self.assertRaisesRegex(InventoryError, "inventoryStatus"):
            adapt_inventory_report(report, INSTALL_ROOT)

        report = valid_report()
        report["checks"][0]["status"] = "FAIL"
        with self.assertRaisesRegex(InventoryError, "non-PASS check"):
            adapt_inventory_report(report, INSTALL_ROOT)

    def test_public_adapter_cannot_accept_a_caller_supplied_report_digest(self):
        with self.assertRaises(TypeError):
            adapt_inventory_report(valid_report(), INSTALL_ROOT, report_sha256="f" * 64)

    def test_report_loader_requires_external_sha256_pin(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            install = base / "install"
            install.mkdir()
            report_path = base / "inventory.json"
            report = valid_report()
            report["installRoot"] = str(install)
            raw = json.dumps(report, separators=(",", ":")).encode("utf-8")
            report_path.write_bytes(raw)
            digest = hashlib.sha256(raw).hexdigest()

            with self.assertRaisesRegex(InventoryError, "external.*SHA256"):
                load_inventory_v43(report_path, str(install), "")

            snapshot = load_inventory_v43(report_path, str(install), digest)
            self.assertEqual(digest, snapshot.report_sha256)

            with self.assertRaisesRegex(InventoryError, "differs from the external pin"):
                load_inventory_v43(report_path, str(install), "0" * 64)

    def test_report_inside_install_root_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            install = Path(temp) / "install"
            install.mkdir()
            report_path = install / "inventory.json"
            report = valid_report()
            report["installRoot"] = str(install)
            raw = json.dumps(report).encode("utf-8")
            report_path.write_bytes(raw)

            with self.assertRaisesRegex(InventoryError, "outside the install root"):
                load_inventory_v43(report_path, str(install), hashlib.sha256(raw).hexdigest())

    def test_candidate_manifest_remains_pending_and_product_actions_disabled(self):
        manifest_path = Path(__file__).resolve().parents[1] / "candidate-manifest-v43.admission-template.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual("OFF", manifest["installBinding"]["requiredVariant"])
        self.assertEqual(2, manifest["installBinding"]["inventorySchemaVersion"])
        self.assertNotIn("fileCount", manifest["installBinding"])
        self.assertFalse(manifest["goPermitted"])
        self.assertTrue(all(value is False for value in manifest["productActions"].values()))
        self.assertEqual("PREPARATION_ONLY_WAITING_FOR_OFF_INSTALL_AND_SOURCE_FREEZE",
                         manifest["candidateState"])
        self.assertIn("PENDING", manifest["installBinding"]["inventoryReportSha256"])
        self.assertIn("PENDING", manifest["candidateEvidencePins"]["offLinkMapReceiptSha256"])
        manifest_text = json.dumps(manifest).casefold()
        self.assertNotIn("v42", manifest_text)
        self.assertNotIn("expectedinventorycount", manifest_text)
        self.assertNotIn("expectedprovidercount", manifest_text)


if __name__ == "__main__":
    unittest.main()
