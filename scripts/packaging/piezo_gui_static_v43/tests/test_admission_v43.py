from __future__ import annotations

import copy
import hashlib
import io
import json
from contextlib import redirect_stderr
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from admission_v43 import (
    AdmissionError,
    REQUIRED_STATIC_TEST_IDS,
    SOURCE_SCOPE,
    SOURCE_SCOPE_SCHEMA,
    _verify_off_link_map_receipt,
    _verify_source_and_build,
    main,
    verify_manifest_file,
)
from inventory_adapter_v43 import REQUIRED_CHECK_IDS


def sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def write_bytes(path: Path, raw: bytes) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)
    return sha256_bytes(raw)


def write_json(path: Path, value) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return write_bytes(path, raw)


def source_tree_sha256(rows):
    canonical = [
        {"path": row["path"], "sizeBytes": row["sizeBytes"], "sha256": row["sha256"].lower()}
        for row in rows
    ]
    canonical.sort(key=lambda row: row["path"].encode("utf-8"))
    raw = json.dumps(canonical, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return sha256_bytes(raw)


def git(repo: Path, *arguments, config=()):
    command = ["git", "-C", str(repo)]
    for key, value in config:
        command.extend(["-c", "{}={}".format(key, value)])
    command.extend(arguments)
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if completed.returncode != 0:
        raise AssertionError("git command failed: {}".format(completed.stderr.decode("utf-8", "replace")))
    return completed.stdout.decode("utf-8", "strict").strip()


def package_tree_sha256(files, directories):
    canonical_files = sorted(
        ({"path": row["path"], "sizeBytes": row["sizeBytes"], "sha256": row["sha256"].lower()}
         for row in files),
        key=lambda row: row["path"].encode("utf-8"),
    )
    raw = json.dumps(
        {"files": canonical_files, "directories": list(directories)},
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return sha256_bytes(raw)


def candidate_identity_sha256(manifest):
    binding = manifest["installBinding"]
    source = manifest["sourceAndAuthorityPins"]
    identity = {
        "schema": "range.pc11-fresh-run-v43-admission-candidate/v1",
        "candidateVersion": "v43",
        "installRoot": binding["installRoot"],
        "inventoryReportSha256": binding["inventoryReportSha256"],
        "sourceFreezeReceiptSha256": source["sourceFreezeReceiptSha256"],
        "rootCommit": source["rootCommit"],
        "sourceTreeSha256": source["sourceTreeSha256"],
        "releaseBuildReceiptSha256": source["releaseBuildReceiptSha256"],
    }
    raw = json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return sha256_bytes(raw)


def create_source_template(source: Path, submodule_source: Path):
    source.mkdir(parents=True)
    submodule_source.mkdir(parents=True)
    git(submodule_source, "init", "--quiet")
    write_bytes(submodule_source / "src/lib.cpp", b"synthetic submodule tracked source")
    git(submodule_source, "add", "src/lib.cpp")
    git(submodule_source, "commit", "--quiet", "-m", "synthetic submodule source",
        config=(("user.name", "v43 test"), ("user.email", "v43@example.invalid")))

    source_file_contents = {
        "src/fea/main.cpp": b"synthetic source bytes",
        "docs/piezo_control_plan.json": b"{\"synthetic\":true}",
        "docs/模型/notes.md": "合成來源檔案".encode("utf-8"),
        "src/ignored.cpp": b"must be excluded by .gitignore",
    }
    for relative, contents in source_file_contents.items():
        write_bytes(source.joinpath(*relative.split("/")), contents)
    write_bytes(source / ".gitignore", b"src/ignored.cpp\n")
    git(source, "init", "--quiet")
    git(source, "add", ".gitignore", "src/fea/main.cpp", "docs/piezo_control_plan.json")
    git(source, "commit", "--quiet", "-m", "synthetic root source",
        config=(("user.name", "v43 test"), ("user.email", "v43@example.invalid")))
    git(source, "-c", "protocol.file.allow=always", "submodule", "add", "--quiet",
        str(submodule_source), "src/range-base-lib")
    git(source, "commit", "--quiet", "-m", "synthetic submodule registration",
        config=(("user.name", "v43 test"), ("user.email", "v43@example.invalid")))
    write_bytes(source / "src/control/measurements.hpp", b"synthetic control source")
    write_bytes(source / "src/range-base-lib/src/dirty.hpp", b"synthetic untracked submodule source")
    return git(source, "rev-parse", "HEAD"), git(submodule_source, "rev-parse", "HEAD")


class V43AdmissionFixture:
    def __init__(self, temp_root: Path, source_template: Path, root_commit: str, module_head: str,
                 extra_install_files=None):
        self.base = temp_root
        self.install = self.base / "off-install"
        self.evidence = self.base / "evidence"
        self.source = self.base / "source"
        self.install.mkdir()
        self.evidence.mkdir()
        shutil.copytree(source_template, self.source)

        installed = {
            "bin/fea.exe": b"synthetic gui image v43 off",
            "bin/fea-solver.exe": b"synthetic solver image v43 off",
            "runtime/python/python.exe": b"synthetic private python v43 off",
            "share/range-workflow/range_workflow/runtime_control.py": b"control provider bytes",
            "share/range-workflow/range_workflow/piezo_native_backend.py": b"native backend bytes",
        }
        for path, contents in (extra_install_files or {}).items():
            installed[path] = contents
        for relative, contents in installed.items():
            write_bytes(self.install.joinpath(*relative.split("/")), contents)
        self.installed = installed

        source_file_contents = {
            "src/fea/main.cpp": b"synthetic source bytes",
            "docs/piezo_control_plan.json": b"{\"synthetic\":true}",
            "src/control/measurements.hpp": b"synthetic control source",
            "src/range-base-lib/src/dirty.hpp": b"synthetic untracked submodule source",
            "src/range-base-lib/src/lib.cpp": b"synthetic submodule tracked source",
        }
        source_file_contents.update({
            "src/range-base-lib/src/lib.cpp": b"synthetic submodule tracked source",
        })
        included_source_paths = (
            "docs/piezo_control_plan.json",
            "src/control/measurements.hpp",
            "src/fea/main.cpp",
            "src/range-base-lib/src/dirty.hpp",
            "src/range-base-lib/src/lib.cpp",
        )
        source_rows = []
        for relative in included_source_paths:
            contents = source_file_contents[relative]
            source_rows.append({
                "path": relative,
                "sizeBytes": len(contents),
                "sha256": sha256_bytes(contents),
            })
        source_rows.sort(key=lambda row: row["path"].encode("utf-8"))
        self.source_rows = source_rows
        source_tree = source_tree_sha256(source_rows)

        ctest_path = self.evidence / "required-ctest-tests.json"
        ctest_sha = write_json(ctest_path, {"requiredTests": ["synthetic-test"]})
        cache_path = self.evidence / "CMakeCache.txt"
        cache_sha = write_bytes(cache_path, b"CMAKE_BUILD_TYPE:STRING=Release\nRANGE_ENABLE_AGENT:BOOL=OFF\n")

        module_rows = [
            {"path": "src/dirty.hpp", "sizeBytes": len(source_file_contents["src/range-base-lib/src/dirty.hpp"]),
             "sha256": sha256_bytes(source_file_contents["src/range-base-lib/src/dirty.hpp"])},
            {"path": "src/lib.cpp", "sizeBytes": len(source_file_contents["src/range-base-lib/src/lib.cpp"]),
             "sha256": sha256_bytes(source_file_contents["src/range-base-lib/src/lib.cpp"])},
        ]
        module_tree = source_tree_sha256(module_rows)
        submodules = [{
            "path": "src/range-base-lib",
            "registeredCommit": module_head,
            "headCommit": module_head,
            "worktreeStatus": "DIRTY_SOURCE_FROZEN",
            "sourceTreeSha256": module_tree,
        }]
        build_path = self.evidence / "release-build-receipt.json"
        build_receipt = {
            "schema": "range.pc11-v43-release-build-receipt/v1",
            "status": "PASS",
            "candidateVersion": "v43",
            "variant": "OFF",
            "configuration": "Release",
            "staticOnly": True,
            "productActions": {
                "guiLaunched": False,
                "savePerformed": False,
                "runPerformed": False,
                "confirmPerformed": False,
            },
            "goPermitted": False,
            "rootCommit": root_commit,
            "submoduleCommits": submodules,
            "sourceTreeSha256": source_tree,
            "requiredCTestManifestSha256": ctest_sha,
            "cmakeCacheSha256": cache_sha,
        }
        build_sha = write_json(build_path, build_receipt)
        freeze_path = self.evidence / "source-freeze.json"
        freeze_receipt = {
            "schema": "range.pc11-v43-source-freeze/v1",
            "status": "PASS",
            "candidateVersion": "v43",
            "variant": "OFF",
            "configuration": "Release",
            "staticOnly": True,
            "productActions": {
                "guiLaunched": False,
                "savePerformed": False,
                "runPerformed": False,
                "confirmPerformed": False,
            },
            "goPermitted": False,
            "sourceRootPath": str(self.source),
            "rootCommit": root_commit,
            "submoduleCommits": submodules,
            "sourceFiles": source_rows,
            "sourceTreeSha256": source_tree,
            "sourceScopeSchema": SOURCE_SCOPE_SCHEMA,
            "sourceScope": SOURCE_SCOPE,
            "requiredCTestManifestPath": str(ctest_path),
            "requiredCTestManifestSha256": ctest_sha,
            "cmakeCachePath": str(cache_path),
            "cmakeCacheSha256": cache_sha,
            "releaseBuildReceiptPath": str(build_path),
            "releaseBuildReceiptSha256": build_sha,
        }
        freeze_sha = write_json(freeze_path, freeze_receipt)
        self.source_freeze_path = freeze_path
        self.release_build_path = build_path

        authority_before = []
        self.authority_paths = []
        for index in range(9):
            path = self.evidence / "original-authority-{:02d}.json".format(index)
            digest = write_bytes(path, ("original authority {}".format(index)).encode("ascii"))
            row = {"kind": "original-authority", "path": str(path), "sha256": digest}
            authority_before.append(row)
            self.authority_paths.append((path, digest))
        authority_receipt = {
            "schema": "range.pc11-v24-static-receipt/v1",
            "originalAuthorityBefore": authority_before,
            "originalAuthorityAfter": authority_before,
        }
        authority_path = self.evidence / "authority-receipt.json"
        authority_sha = write_json(authority_path, authority_receipt)
        extra_authority = self.evidence / "additional-original-authority.json"
        extra_authority_sha = write_bytes(extra_authority, b"tenth original authority")
        self.authority_paths.append((extra_authority, extra_authority_sha))

        provider_root = "share/range-workflow/range_workflow"
        inventory_rows = []
        directories = set()
        for relative, contents in installed.items():
            for index in range(1, len(relative.split("/"))):
                directories.add("/".join(relative.split("/")[:index]))
            inventory_rows.append({
                "path": relative,
                "sizeBytes": len(contents),
                "sha256": sha256_bytes(contents),
            })
        inventory_rows.sort(key=lambda row: (row["path"].casefold(), row["path"]))
        directories = sorted(directories, key=lambda value: (value.casefold(), value))
        inventory_report = {
            "schemaVersion": 2,
            "inventoryStatus": "PASS",
            "releaseQualificationStatus": "INCOMPLETE_LINK_MAP_NOT_SUPPLIED",
            "variant": "OFF",
            "installRoot": str(self.install),
            "fileCount": len(inventory_rows),
            "files": inventory_rows,
            "directories": directories,
            "checks": [{"id": check_id, "status": "PASS"} for check_id in sorted(REQUIRED_CHECK_IDS)],
        }
        self.inventory_path = self.evidence / "inventory-v43-off-v2.json"
        self.inventory_sha = write_json(self.inventory_path, inventory_report)

        package_receipt = self.evidence / "package-tree-receipt.json"
        static_receipt = self.evidence / "static-tests-receipt.json"
        off_link_map_receipt = self.evidence / "off-link-map-v44.json"
        package_sha = "0" * 64
        static_sha = "0" * 64
        off_link_map_sha = "0" * 64

        file_by_path = {row["path"]: row for row in inventory_rows}
        self.manifest = {
            "schema": "range.pc11-fresh-run-v43-admission-candidate/v1",
            "candidateVersion": "v43",
            "candidateState": "STATIC_CANDIDATE_PENDING_REVIEW_AND_ROOT_DECISION",
            "goPermitted": False,
            "productActions": {
                "guiLaunched": False,
                "savePerformed": False,
                "runPerformed": False,
                "confirmPerformed": False,
            },
            "installBinding": {
                "requiredVariant": "OFF",
                "installRoot": str(self.install),
                "inventoryReportPath": str(self.inventory_path),
                "inventoryReportSha256": self.inventory_sha,
                "inventorySchemaVersion": 2,
                "inventoryStatus": "PASS",
                "fileCountSource": "inventoryReport.fileCount == length(inventoryReport.files)",
            },
            "installedPathLayout": {
                "guiExecutableRelativePath": "bin/fea.exe",
                "privatePythonRelativePath": "runtime/python/python.exe",
                "solverExecutableRelativePath": "bin/fea-solver.exe",
                "providerRootRelativePath": provider_root,
                "productionControlRelativePath": provider_root + "/runtime_control.py",
                "nativeBackendRelativePath": provider_root + "/piezo_native_backend.py",
                "guiExecutableSha256": file_by_path["bin/fea.exe"]["sha256"],
                "privatePythonSha256": file_by_path["runtime/python/python.exe"]["sha256"],
                "solverExecutableSha256": file_by_path["bin/fea-solver.exe"]["sha256"],
                "productionControlSha256": file_by_path[provider_root + "/runtime_control.py"]["sha256"],
                "nativeBackendSha256": file_by_path[provider_root + "/piezo_native_backend.py"]["sha256"],
            },
            "sourceAndAuthorityPins": {
                "sourceRootPath": str(self.source),
                "sourceFreezeReceiptPath": str(freeze_path),
                "sourceFreezeReceiptSha256": freeze_sha,
                "rootCommit": build_receipt["rootCommit"],
                "submoduleCommits": submodules,
                "sourceTreeSha256": source_tree,
                "requiredCTestManifestPath": str(ctest_path),
                "requiredCTestManifestSha256": ctest_sha,
                "cmakeCachePath": str(cache_path),
                "cmakeCacheSha256": cache_sha,
                "releaseBuildReceiptPath": str(build_path),
                "releaseBuildReceiptSha256": build_sha,
                "authorityBinding": {
                    "authorityReceiptPath": str(authority_path),
                    "authorityReceiptSha256": authority_sha,
                    "additionalOriginalAuthorities": [
                        {"path": str(extra_authority), "sha256": extra_authority_sha},
                    ],
                },
            },
            "candidateEvidencePins": {
                "offLinkMapReceiptPath": str(off_link_map_receipt),
                "offLinkMapReceiptSha256": off_link_map_sha,
                "packageTreeReceiptPath": str(package_receipt),
                "packageTreeReceiptSha256": package_sha,
                "requiredStaticTestIds": list(REQUIRED_STATIC_TEST_IDS),
                "staticTestReceipts": [
                    {"name": "v43-static-suite", "path": str(static_receipt), "sha256": static_sha},
                ],
            },
            "workflowBoundary": {
                "staticReadinessDoesNotGrantProductGo": True,
                "externalIndependentReviewRequired": True,
                "separateRootTechnicalDecisionRequired": True,
                "goSealRequiredBeforeAnyProductAction": True,
            },
        }
        synthetic_map_path = self.evidence / "fea-link.map"
        synthetic_map_sha = write_bytes(synthetic_map_path, b"synthetic clean OFF map receipt input")
        gui_relative = self.manifest["installedPathLayout"]["guiExecutableRelativePath"]
        gui_row = file_by_path[gui_relative]
        build_pe_path = self.evidence / "Release" / "fea.exe"
        build_pe_path.parent.mkdir(parents=True, exist_ok=True)
        build_pe_path.write_bytes((self.install / Path(*gui_relative.split("/"))).read_bytes())
        pe_identity = {
            "sizeBytes": gui_row["sizeBytes"],
            "sha256": gui_row["sha256"],
            "machine": "AMD64",
            "coffTimestamp": "1234abcd",
        }
        off_link_map_body = {
            "schemaVersion": 1,
            "receiptType": "AGENT_OFF_MSVC_LINK_MAP_GATE",
            "linkMapGateStatus": "PASS",
            "variant": "OFF",
            "goPermitted": False,
            "externalPins": {
                "map": {
                    "path": str(synthetic_map_path),
                    "sizeBytes": synthetic_map_path.stat().st_size,
                    "sha256": synthetic_map_sha,
                },
                "sourceProof": {
                    "path": str(freeze_path),
                    "sizeBytes": freeze_path.stat().st_size,
                    "sha256": freeze_sha,
                },
                "inventoryReport": {
                    "path": str(self.inventory_path),
                    "sizeBytes": self.inventory_path.stat().st_size,
                    "sha256": self.inventory_sha,
                },
            },
            "peBinding": {
                "build": {"path": str(build_pe_path), **pe_identity},
                "installed": {
                    "path": str(self.install / Path(*gui_relative.split("/"))),
                    **pe_identity,
                },
                "byteIdentical": True,
                "mapTimestampMatchesCoffTimestamp": True,
                "mapHeaderTimestamp": "1234abcd",
            },
            "pinnedInventoryReport": {
                "variant": "OFF",
                "inventoryStatus": "PASS",
                "releaseQualificationStatus": inventory_report["releaseQualificationStatus"],
                "reportWasModified": False,
            },
            "map": {
                "timestamp": "1234abcd",
                "forbiddenRowCount": 0,
                "forbiddenRows": [],
            },
        }
        off_link_map_sha = write_json(off_link_map_receipt, off_link_map_body)
        self.off_link_map_receipt_path = off_link_map_receipt
        self.manifest["candidateEvidencePins"]["offLinkMapReceiptSha256"] = off_link_map_sha
        identity_sha = candidate_identity_sha256(self.manifest)
        package_files = sorted(
            inventory_rows,
            key=lambda row: row["path"].encode("utf-8"),
        )
        package_receipt_body = {
            "schema": "range.pc11-v43-package-tree-receipt/v1",
            "status": "PASS",
            "candidateVersion": "v43",
            "variant": "OFF",
            "staticOnly": True,
            "goPermitted": False,
            "productActions": {key: False for key in (
                "guiLaunched", "savePerformed", "runPerformed", "confirmPerformed")},
            "candidateManifestIdentitySha256": identity_sha,
            "sourceFreezeReceiptSha256": freeze_sha,
            "rootCommit": root_commit,
            "submoduleCommits": submodules,
            "sourceTreeSha256": source_tree,
            "requiredCTestManifestSha256": ctest_sha,
            "cmakeCacheSha256": cache_sha,
            "releaseBuildReceiptSha256": build_sha,
            "inventoryReportSha256": self.inventory_sha,
            "files": package_files,
            "directories": directories,
            "fileCount": len(package_files),
            "directoryCount": len(directories),
            "treeDigest": package_tree_sha256(package_files, directories),
        }
        package_sha = write_json(package_receipt, package_receipt_body)
        self.package_receipt_path = package_receipt
        static_receipt_body = {
            "schema": "range.pc11-v43-static-test-receipt/v1",
            "status": "PASS",
            "candidateVersion": "v43",
            "variant": "OFF",
            "staticOnly": True,
            "goPermitted": False,
            "productActions": {key: False for key in (
                "guiLaunched", "savePerformed", "runPerformed", "confirmPerformed")},
            "candidateManifestIdentitySha256": identity_sha,
            "sourceTreeSha256": source_tree,
            "inventoryReportSha256": self.inventory_sha,
            "packageTreeReceiptSha256": package_sha,
            "requiredTestIds": list(REQUIRED_STATIC_TEST_IDS),
            "results": [{"testId": test_id, "status": "PASS"} for test_id in REQUIRED_STATIC_TEST_IDS],
        }
        static_sha = write_json(static_receipt, static_receipt_body)
        self.static_receipt_path = static_receipt
        self.manifest["candidateEvidencePins"]["packageTreeReceiptSha256"] = package_sha
        self.manifest["candidateEvidencePins"]["staticTestReceipts"][0]["sha256"] = static_sha
        self.manifest_path = self.evidence / "candidate-manifest.json"
        self.manifest_sha = write_json(self.manifest_path, self.manifest)

    def run(self):
        return verify_manifest_file(self.manifest_path, self.manifest_sha, self.inventory_sha)

    def rewrite_manifest(self):
        self.manifest_sha = write_json(self.manifest_path, self.manifest)

    def rebind_source_freeze(self, receipt):
        digest = write_json(self.source_freeze_path, receipt)
        self.manifest["sourceAndAuthorityPins"]["sourceFreezeReceiptSha256"] = digest
        return digest

    def rebind_release_build(self, receipt):
        digest = write_json(self.release_build_path, receipt)
        pins = self.manifest["sourceAndAuthorityPins"]
        freeze = json.loads(self.source_freeze_path.read_text(encoding="utf-8"))
        freeze["releaseBuildReceiptSha256"] = digest
        freeze_digest = write_json(self.source_freeze_path, freeze)
        pins["releaseBuildReceiptSha256"] = digest
        pins["sourceFreezeReceiptSha256"] = freeze_digest
        return digest

    def replace_package_receipt(self, value):
        digest = write_json(self.package_receipt_path, value)
        self.manifest["candidateEvidencePins"]["packageTreeReceiptSha256"] = digest
        self.rewrite_manifest()

    def replace_static_receipt(self, value):
        digest = write_json(self.static_receipt_path, value)
        self.manifest["candidateEvidencePins"]["staticTestReceipts"][0]["sha256"] = digest
        self.rewrite_manifest()

    def replace_off_link_map_receipt(self, value):
        digest = write_json(self.off_link_map_receipt_path, value)
        self.manifest["candidateEvidencePins"]["offLinkMapReceiptSha256"] = digest
        self.rewrite_manifest()


class AdmissionV43Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_template_temp = tempfile.TemporaryDirectory()
        source_template_base = Path(cls.source_template_temp.name)
        cls.source_template = source_template_base / "source-template"
        cls.module_template = source_template_base / "submodule-source"
        cls.root_commit, cls.module_head = create_source_template(cls.source_template, cls.module_template)

    @classmethod
    def tearDownClass(cls):
        cls.source_template_temp.cleanup()

    def _fixture(self, extra_files=None):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        return V43AdmissionFixture(
            Path(temp.name), self.source_template, self.root_commit, self.module_head, extra_files)

    def test_dynamic_off_install_and_provider_set_pass_without_product_go(self):
        fixture = self._fixture({
            "share/range-workflow/range_workflow/extra_provider.py": b"extra provider",
            "doc/readme.txt": b"extra install file",
        })

        result = fixture.run()

        self.assertEqual("STATIC_OFF_INSTALL_ADMISSION_PASS_NO_PRODUCT_GO", result["status"])
        self.assertFalse(result["goPermitted"])
        self.assertTrue(result["staticAdmissionEvaluated"])
        self.assertFalse(result["guiAdmissionEvaluated"])
        self.assertTrue(all(value is False for value in result["productActions"].values()))
        self.assertEqual("PASS", result["offLinkMapGateStatus"])
        self.assertEqual(len(fixture.installed), result["install"]["inventoryFileCount"])
        self.assertEqual(len(result["install"]["providerRelativePaths"]), result["install"]["providerFileCount"])
        self.assertIn("extra_provider.py", result["install"]["providerRelativePaths"])
        self.assertEqual(10, result["originalAuthorityCount"])
        self.assertEqual(1, result["sourceAndBuild"]["submoduleCount"])
        self.assertEqual(5, result["sourceAndBuild"]["sourceFileCount"])
        self.assertEqual(list(REQUIRED_STATIC_TEST_IDS), result["staticTestIds"])
        self.assertEqual("INCOMPLETE_LINK_MAP_NOT_SUPPLIED", result["install"]["releaseQualificationStatus"])

    def test_missing_off_link_map_pin_cannot_pass_full_admission(self):
        fixture = self._fixture()
        fixture.manifest["candidateEvidencePins"].pop("offLinkMapReceiptPath")
        fixture.manifest["candidateEvidencePins"].pop("offLinkMapReceiptSha256")
        fixture.rewrite_manifest()

        with self.assertRaisesRegex(AdmissionError, "must externally pin the OFF link-map v44 receipt"):
            fixture.run()

    def test_fail_off_link_map_receipt_cannot_pass_full_admission(self):
        fixture = self._fixture()
        receipt = json.loads(fixture.off_link_map_receipt_path.read_text(encoding="utf-8"))
        receipt["linkMapGateStatus"] = "FAIL"
        fixture.replace_off_link_map_receipt(receipt)

        with self.assertRaisesRegex(AdmissionError, "linkMapGateStatus must be PASS"):
            fixture.run()

    def test_off_link_map_receipt_schema_and_identity_fields_are_checked_independently(self):
        fixture = self._fixture()
        evidence = fixture.manifest["candidateEvidencePins"]
        valid = json.loads(fixture.off_link_map_receipt_path.read_text(encoding="utf-8"))
        inventory_report = json.loads(fixture.inventory_path.read_text(encoding="utf-8"))
        mutations = (
            ("receiptType", "BAD_TYPE", "receiptType must be AGENT_OFF_MSVC_LINK_MAP_GATE"),
            ("schemaVersion", 2, "schemaVersion must be 1"),
            ("linkMapGateStatus", "FAIL", "linkMapGateStatus must be PASS"),
            ("variant", "ON", "variant must be OFF"),
            ("goPermitted", True, "goPermitted must be false"),
        )
        for field, value, expected_error in mutations:
            with self.subTest(field=field):
                mutated = copy.deepcopy(valid)
                mutated[field] = value
                fixture.replace_off_link_map_receipt(mutated)
                with self.assertRaisesRegex(AdmissionError, expected_error):
                    _verify_off_link_map_receipt(evidence, fixture.manifest, inventory_report)

        mutations = (
            ("inventoryReport.path", "path differs from the manifest"),
            ("inventoryReport.sha256", "SHA256 differs from the manifest"),
            ("pinnedInventoryReport.variant", "pinned inventory variant must match OFF"),
            ("pinnedInventoryReport.inventoryStatus", "inventoryStatus must be PASS"),
            ("pinnedInventoryReport.releaseQualificationStatus", "releaseQualificationStatus differs"),
            ("pinnedInventoryReport.reportWasModified", "reportWasModified must be false"),
            ("sourceProof.path", "sourceProof path differs from the source freeze receipt"),
            ("sourceProof.sha256", "sourceProof SHA256 differs from the source freeze receipt"),
            ("peBinding.byteIdentical", "byteIdentical must be true"),
            ("peBinding.mapTimestampMatchesCoffTimestamp", "mapTimestampMatchesCoffTimestamp must be true"),
            ("peBinding.installed.path", "installed PE path differs from the manifest GUI executable"),
            ("peBinding.installed.sha256", "installed PE SHA256 differs from the manifest GUI executable"),
            ("peBinding.build.sha256", "build PE SHA256 differs from the installed GUI executable"),
            ("map.forbiddenRowCount", "forbiddenRowCount must be zero"),
            ("map.forbiddenRows", "forbiddenRows must be empty"),
        )
        for field, expected_error in mutations:
            with self.subTest(field=field):
                mutated = copy.deepcopy(valid)
                if field == "inventoryReport.path":
                    mutated["externalPins"]["inventoryReport"]["path"] = str(fixture.evidence / "wrong-report.json")
                elif field == "inventoryReport.sha256":
                    mutated["externalPins"]["inventoryReport"]["sha256"] = "0" * 64
                elif field.startswith("pinnedInventoryReport."):
                    key = field.split(".", 1)[1]
                    mutated["pinnedInventoryReport"][key] = {
                        "variant": "ON",
                        "inventoryStatus": "FAIL",
                        "releaseQualificationStatus": "GO",
                        "reportWasModified": True,
                    }[key]
                elif field == "sourceProof.path":
                    mutated["externalPins"]["sourceProof"]["path"] = str(fixture.evidence / "wrong-source.json")
                elif field == "sourceProof.sha256":
                    mutated["externalPins"]["sourceProof"]["sha256"] = "0" * 64
                elif field.startswith("peBinding."):
                    suffix = field.split(".", 1)[1]
                    if suffix in ("byteIdentical", "mapTimestampMatchesCoffTimestamp"):
                        mutated["peBinding"][suffix] = False
                    else:
                        section, key = suffix.split(".", 1)
                        mutated["peBinding"][section][key] = (
                            str(fixture.evidence / "wrong-fea.exe") if key == "path" else "0" * 64)
                elif field == "map.forbiddenRowCount":
                    mutated["map"]["forbiddenRowCount"] = 1
                else:
                    mutated["map"]["forbiddenRows"] = [{"symbol": "forbidden"}]
                fixture.replace_off_link_map_receipt(mutated)
                with self.assertRaisesRegex(AdmissionError, expected_error):
                    _verify_off_link_map_receipt(evidence, fixture.manifest, inventory_report)

    def test_tampered_or_arbitrary_pinned_link_map_receipt_is_rejected(self):
        fixture = self._fixture()
        fixture.replace_off_link_map_receipt({"linkMapGateStatus": "PASS"})
        with self.assertRaisesRegex(AdmissionError, "receiptType must be AGENT_OFF_MSVC_LINK_MAP_GATE"):
            fixture.run()

        fixture = self._fixture()
        original = json.loads(fixture.off_link_map_receipt_path.read_text(encoding="utf-8"))
        original["linkMapGateStatus"] = "FAIL"
        write_json(fixture.off_link_map_receipt_path, original)
        with self.assertRaisesRegex(AdmissionError, "OFF link-map v44 receipt SHA256 differs from the external pin"):
            fixture.run()

    def test_external_manifest_and_inventory_pins_are_required(self):
        fixture = self._fixture()

        with self.assertRaisesRegex(AdmissionError, "manifest SHA256 differs"):
            verify_manifest_file(fixture.manifest_path, "0" * 64, fixture.inventory_sha)
        with self.assertRaisesRegex(AdmissionError, "external inventory SHA256 differs"):
            verify_manifest_file(fixture.manifest_path, fixture.manifest_sha, "0" * 64)

    def test_installed_file_or_empty_directory_added_after_inventory_is_rejected(self):
        fixture = self._fixture()
        (fixture.install / "new-empty-dir").mkdir()

        with self.assertRaisesRegex(AdmissionError, "directory set mismatch"):
            fixture.run()

    def test_variant_on_is_rejected_even_with_a_matching_report_pin(self):
        fixture = self._fixture()
        inventory = json.loads(fixture.inventory_path.read_text(encoding="utf-8"))
        inventory["variant"] = "ON"
        fixture.inventory_sha = write_json(fixture.inventory_path, inventory)
        fixture.manifest["installBinding"]["inventoryReportSha256"] = fixture.inventory_sha
        fixture.rewrite_manifest()

        with self.assertRaisesRegex(AdmissionError, "schema-v2 OFF inventory/tree review failed"):
            fixture.run()

    def test_installed_executable_pin_must_match_the_inventory(self):
        fixture = self._fixture()
        fixture.manifest["installedPathLayout"]["solverExecutableSha256"] = "f" * 64
        fixture.rewrite_manifest()

        with self.assertRaisesRegex(AdmissionError, "solverExecutableSha256"):
            fixture.run()

    def test_source_file_bytes_are_checked_against_the_freeze_report(self):
        fixture = self._fixture()
        (fixture.source / "src" / "fea" / "main.cpp").write_bytes(b"mutated source")

        with self.assertRaisesRegex(AdmissionError, "source scope file rows differ from freeze receipt"):
            fixture.run()

    def test_source_freeze_receipt_metadata_is_validated_field_by_field(self):
        fixture = self._fixture()
        source_pins = fixture.manifest["sourceAndAuthorityPins"]
        valid = json.loads(fixture.source_freeze_path.read_text(encoding="utf-8"))
        mutations = (
            ("schema", "range.pc11-v43-source-freeze/invalid", "source freeze receipt schema mismatch"),
            ("status", "SOURCE_FREEZE_FAIL", "source freeze receipt status must be PASS"),
            ("candidateVersion", "v42", "source freeze receipt candidateVersion must be v43"),
            ("variant", "ON", "source freeze receipt variant must be OFF"),
            ("configuration", "Debug", "source freeze receipt configuration must be Release"),
            ("staticOnly", False, "source freeze receipt staticOnly must be true"),
            ("goPermitted", True, "source freeze receipt goPermitted must be false"),
        )
        for field, value, expected_error in mutations:
            with self.subTest(field=field):
                mutated = copy.deepcopy(valid)
                mutated[field] = value
                fixture.rebind_source_freeze(mutated)
                with self.assertRaisesRegex(AdmissionError, expected_error):
                    _verify_source_and_build(source_pins)

        with self.subTest(field="productActions"):
            mutated = copy.deepcopy(valid)
            mutated["productActions"]["guiLaunched"] = True
            fixture.rebind_source_freeze(mutated)
            with self.assertRaisesRegex(AdmissionError, "source freeze receipt must declare every product action false"):
                _verify_source_and_build(source_pins)

    def test_release_build_receipt_metadata_is_validated_field_by_field(self):
        fixture = self._fixture()
        source_pins = fixture.manifest["sourceAndAuthorityPins"]
        valid = json.loads(fixture.release_build_path.read_text(encoding="utf-8"))
        mutations = (
            ("schema", "range.pc11-v43-release-build-receipt/invalid", "Release build receipt schema mismatch"),
            ("status", "FAIL", "Release build receipt status must be PASS"),
            ("candidateVersion", "v42", "Release build receipt candidateVersion must be v43"),
            ("variant", "ON", "Release build receipt variant must be OFF"),
            ("configuration", "Debug", "Release build receipt configuration must be Release"),
            ("staticOnly", False, "Release build receipt staticOnly must be true"),
            ("goPermitted", True, "Release build receipt goPermitted must be false"),
        )
        for field, value, expected_error in mutations:
            with self.subTest(field=field):
                mutated = copy.deepcopy(valid)
                mutated[field] = value
                fixture.rebind_release_build(mutated)
                with self.assertRaisesRegex(AdmissionError, expected_error):
                    _verify_source_and_build(source_pins)

        with self.subTest(field="productActions"):
            mutated = copy.deepcopy(valid)
            mutated["productActions"]["guiLaunched"] = True
            fixture.rebind_release_build(mutated)
            with self.assertRaisesRegex(AdmissionError, "Release build receipt must declare every product action false"):
                _verify_source_and_build(source_pins)

    def test_re_pinned_source_freeze_failure_cannot_pass_full_admission(self):
        fixture = self._fixture()
        freeze = json.loads(fixture.source_freeze_path.read_text(encoding="utf-8"))
        freeze["status"] = "SOURCE_FREEZE_FAIL"
        fixture.rebind_source_freeze(freeze)
        fixture.rewrite_manifest()

        with self.assertRaisesRegex(AdmissionError, "source freeze receipt status must be PASS"):
            fixture.run()

    def test_re_pinned_release_build_failure_cannot_pass_full_admission(self):
        fixture = self._fixture()
        build = json.loads(fixture.release_build_path.read_text(encoding="utf-8"))
        build["status"] = "FAIL"
        fixture.rebind_release_build(build)
        fixture.rewrite_manifest()

        with self.assertRaisesRegex(AdmissionError, "Release build receipt status must be PASS"):
            fixture.run()

    def test_new_build_relevant_source_after_freeze_is_rejected(self):
        fixture = self._fixture()
        write_bytes(fixture.source / "src/fea/new-after-freeze.cpp", b"new source after freeze")
        write_bytes(
            fixture.source / "src/range-base-lib/src/new-after-freeze.hpp",
            b"new recursive submodule source after freeze",
        )

        with self.assertRaisesRegex(AdmissionError, "source scope file set mismatch"):
            fixture.run()

    def test_build_like_file_outside_frozen_scope_is_rejected(self):
        fixture = self._fixture()
        write_bytes(fixture.source / "unscoped-new-module.cpp", b"build relevant")

        with self.assertRaisesRegex(AdmissionError, "build-like source file is outside"):
            fixture.run()

    def test_arbitrary_pinned_package_file_cannot_pass_as_a_tree_receipt(self):
        fixture = self._fixture()
        fixture.replace_package_receipt({"status": "PASS", "files": []})

        with self.assertRaisesRegex(AdmissionError, "package-tree receipt must use the v43 schema"):
            fixture.run()

    def test_arbitrary_pinned_static_file_cannot_pass_as_a_static_suite(self):
        fixture = self._fixture()
        fixture.replace_static_receipt({"status": "PASS", "tests": 19})

        with self.assertRaisesRegex(AdmissionError, "aggregate static-test receipt schema mismatch"):
            fixture.run()

    def test_static_receipt_non_pass_status_and_incomplete_test_set_are_rejected(self):
        fixture = self._fixture()
        receipt = json.loads(fixture.static_receipt_path.read_text(encoding="utf-8"))
        receipt["status"] = "STATIC_INCOMPLETE"
        fixture.replace_static_receipt(receipt)
        captured = io.StringIO()
        with redirect_stderr(captured):
            exit_code = main([
                "--manifest", str(fixture.manifest_path),
                "--manifest-sha256", fixture.manifest_sha,
                "--inventory-sha256", fixture.inventory_sha,
            ])
        self.assertEqual(2, exit_code)
        self.assertEqual("STATIC_INCOMPLETE", json.loads(captured.getvalue())["status"])

        fixture = self._fixture()
        receipt = json.loads(fixture.static_receipt_path.read_text(encoding="utf-8"))
        receipt["requiredTestIds"].pop()
        fixture.replace_static_receipt(receipt)

        with self.assertRaisesRegex(AdmissionError, "requiredTestIds differ from the v43 static contract"):
            fixture.run()

    def test_package_receipt_must_match_candidate_tree_identity_and_inventory(self):
        fixture = self._fixture()
        receipt = json.loads(fixture.package_receipt_path.read_text(encoding="utf-8"))
        receipt["inventoryReportSha256"] = "0" * 64
        fixture.replace_package_receipt(receipt)

        with self.assertRaisesRegex(AdmissionError, "package-tree receipt inventoryReportSha256 differs"):
            fixture.run()

    def test_release_build_receipt_must_bind_to_off_release_and_frozen_source(self):
        fixture = self._fixture()
        build_path = Path(fixture.manifest["sourceAndAuthorityPins"]["releaseBuildReceiptPath"])
        receipt = json.loads(build_path.read_text(encoding="utf-8"))
        receipt["variant"] = "ON"
        new_build_sha = write_json(build_path, receipt)
        freeze_path = Path(fixture.manifest["sourceAndAuthorityPins"]["sourceFreezeReceiptPath"])
        freeze = json.loads(freeze_path.read_text(encoding="utf-8"))
        freeze["releaseBuildReceiptSha256"] = new_build_sha
        new_freeze_sha = write_json(freeze_path, freeze)
        fixture.manifest["sourceAndAuthorityPins"]["releaseBuildReceiptSha256"] = new_build_sha
        fixture.manifest["sourceAndAuthorityPins"]["sourceFreezeReceiptSha256"] = new_freeze_sha
        fixture.rewrite_manifest()

        with self.assertRaisesRegex(AdmissionError, "Release build receipt variant must be OFF"):
            fixture.run()

    def test_authority_set_must_have_ten_unique_report_derived_paths(self):
        fixture = self._fixture()
        fixture.manifest["sourceAndAuthorityPins"]["authorityBinding"]["additionalOriginalAuthorities"] = []
        fixture.rewrite_manifest()

        with self.assertRaisesRegex(AdmissionError, "exactly 10 distinct report paths"):
            fixture.run()

    def test_changed_original_authority_is_rejected(self):
        fixture = self._fixture()
        path, _ = fixture.authority_paths[-1]
        path.write_bytes(b"changed authority")

        with self.assertRaisesRegex(AdmissionError, "original authority bytes differ"):
            fixture.run()

    def test_manifest_cannot_turn_on_go_or_product_actions(self):
        fixture = self._fixture()
        fixture.manifest["goPermitted"] = True
        fixture.rewrite_manifest()
        with self.assertRaisesRegex(AdmissionError, "goPermitted=false"):
            fixture.run()

        fixture.manifest["goPermitted"] = False
        fixture.manifest["productActions"]["runPerformed"] = True
        fixture.rewrite_manifest()
        with self.assertRaisesRegex(AdmissionError, "cannot allow a product action"):
            fixture.run()

    def test_template_is_pending_and_does_not_encode_file_or_provider_counts(self):
        template_path = Path(__file__).resolve().parents[1] / "candidate-manifest-v43.admission-template.json"
        template = json.loads(template_path.read_text(encoding="utf-8"))

        self.assertEqual("OFF", template["installBinding"]["requiredVariant"])
        self.assertEqual(2, template["installBinding"]["inventorySchemaVersion"])
        self.assertEqual(
            "PENDING_OFF_LINK_MAP_V44_RECEIPT_PATH",
            template["candidateEvidencePins"]["offLinkMapReceiptPath"],
        )
        self.assertEqual(
            "PENDING_EXTERNAL_SHA256_PIN",
            template["candidateEvidencePins"]["offLinkMapReceiptSha256"],
        )
        self.assertFalse(template["goPermitted"])
        self.assertTrue(all(value is False for value in template["productActions"].values()))
        text = json.dumps(template).casefold()
        self.assertNotIn("expectedinventorycount", text)
        self.assertNotIn("expectedprovidercount", text)


if __name__ == "__main__":
    unittest.main()
