import json
import hashlib
import locale
import tempfile
import unittest
from pathlib import Path
from unittest import mock
import subprocess

import inventory_v43

DEFAULT_HARNESS_SCRIPT = object()


class InventoryFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="v43-inventory-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build-tree"
        self.release = self.build / "Release"
        self.install = self.root / "clean-install"
        self.redist = self.root / "redist" / "vcomp140.dll"
        self.dumpbin = self.root / "visual-studio" / "dumpbin.exe"
        self.release.mkdir(parents=True)
        self.install.mkdir(parents=True)
        self.redist.parent.mkdir(parents=True)
        self.dumpbin.parent.mkdir(parents=True)
        self.redist.write_bytes(b"matching supplied redist vcomp")
        self.dumpbin.write_bytes(b"synthetic dumpbin placeholder")
        self.dependencies = ["KERNEL32.dll", "Qt6Core.dll", "VCOMP140.DLL"]

        for installed_relative in sorted(inventory_v43.PROJECT_RELEASE_PEER_REQUIRED_PATHS):
            executable = installed_relative.rsplit("/", 1)[-1]
            content = b"synthetic project PE bytes for " + executable.encode("ascii")
            build_exe = self.release / executable
            build_exe.write_bytes(content)
            installed_exe = self.install.joinpath(*installed_relative.split("/"))
            installed_exe.parent.mkdir(parents=True, exist_ok=True)
            installed_exe.write_bytes(content)

        (self.install / "bin" / "vcomp140.dll").write_bytes(self.redist.read_bytes())
        (self.install / "bin" / "Qt6Core.dll").write_bytes(b"synthetic Qt DLL bytes")

        qt_root = self.install / "doc" / "licenses" / "Qt-sdk"
        for notice in inventory_v43.QT_SUPPLIER_NOTICES:
            (qt_root / notice).parent.mkdir(parents=True, exist_ok=True)
            (qt_root / notice).write_text("synthetic supplier notice\n", encoding="utf-8")
        for page in inventory_v43.QT_ATTRIBUTION_PAGES:
            attribution = qt_root / "attribution" / Path(page)
            attribution.parent.mkdir(parents=True, exist_ok=True)
            attribution.write_text("synthetic attribution page\n", encoding="utf-8")
        sbom = qt_root / "sbom" / "supplier.spdx.json"
        sbom.parent.mkdir(parents=True, exist_ok=True)
        sbom.write_text("{}\n", encoding="utf-8")
        tetgen = self.install / "doc" / "licenses" / "TetGen" / "LICENSE"
        tetgen.parent.mkdir(parents=True, exist_ok=True)
        tetgen.write_text("synthetic TetGen license record\n", encoding="utf-8")

    @staticmethod
    def dumpbin_result(dependencies, delay_dependencies=None):
        output = ("Dump of file synthetic.exe\n\n"
                  "File Type: EXECUTABLE IMAGE\n\n"
                  "  Image has the following dependencies:\n\n" + "".join(
                      "  {}\n".format(name) for name in dependencies))
        if delay_dependencies is not None:
            output += ("\n  Image has the following delay load dependencies:\n\n" + "".join(
                "  {}\n".format(name) for name in delay_dependencies))
        output += "\n  Summary\n        1000 .text\n"
        return subprocess.CompletedProcess([], 0, output, "")

    def run_audit(self, variant="OFF", dumpbin_runner=None, supplier_exe_mappings=None,
                  link_map_value=None, gui_runtime_receipt_value=None,
                  gui_harness_script_value=DEFAULT_HARNESS_SCRIPT):
        if dumpbin_runner is None:
            dumpbin_runner = lambda exe: self.dumpbin_result(self.dependencies)
        if gui_harness_script_value is DEFAULT_HARNESS_SCRIPT:
            gui_harness_script_value = getattr(self, "gui_harness_script", None)
        return inventory_v43.audit_inventory(
            self.build, self.install, variant, self.redist, self.dumpbin,
            supplier_exe_mappings=supplier_exe_mappings,
            dumpbin_runner=dumpbin_runner,
            link_map_value=link_map_value,
            gui_runtime_receipt_value=gui_runtime_receipt_value,
            gui_harness_script_value=gui_harness_script_value,
        )

    def write_matching_link_map(self):
        link_map = self.release / "fea.map"
        link_map.write_bytes(
            b" fea\r\n\r\n Timestamp is 12345678\r\n\r\n Preferred load address is 0000000140000000\r\n\r\n"
            b" Publics by Value\r\n 0001:0000 ??symbol 0000000140000000 f TetGen:tetgen.obj\r\n\r\n"
            b" Static symbols\r\n 0001:0000 ??mesh 0000000140000010 f range-model-lib:rml_tetgen.obj\r\n"
        )
        return link_map

    def write_passing_gui_receipt(self, mutate=None):
        executable = self.install / "bin" / "fea.exe"
        self.gui_harness_script = self.root / "qualify_installed_gui_runtime.ps1"
        self.gui_harness_script.write_text("# synthetic GUI harness fixture\n", encoding="utf-8")
        (self.install / "bin" / "Qt6Gui.dll").write_bytes(b"synthetic Qt GUI DLL bytes")
        (self.install / "bin" / "Qt6Widgets.dll").write_bytes(b"synthetic Qt Widgets DLL bytes")
        platform_plugin = self.install / "bin" / "platforms" / "qwindows.dll"
        platform_plugin.parent.mkdir(parents=True, exist_ok=True)
        platform_plugin.write_bytes(b"synthetic Qt platform plugin")
        evidence = self.root / "runtime-evidence"
        stdout = evidence / "stdout.txt"
        stderr = evidence / "stderr.txt"
        application_log = evidence / "profile" / "application-data" / "log" / "application.log"
        application_log.parent.mkdir(parents=True, exist_ok=True)
        stdout.write_bytes(b"synthetic GUI stdout\n")
        stderr.write_bytes(b"")
        application_log.write_bytes(b"synthetic GUI application log\n")
        receipt = {
            "schemaVersion": 2,
            "harnessScriptPath": str(self.gui_harness_script),
            "harnessScriptSha256": hashlib.sha256(self.gui_harness_script.read_bytes()).hexdigest(),
            "executable": str(executable),
            "executableSha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "outcome": "passed",
            "respondingAtCapture": True,
            "requiredQtModulesFromInstall": True,
            "exitedAfterGracefulClose": True,
            "exitCode": 0,
            "qtLoadErrorScan": {"complete": True, "matchesFound": False},
            "outputFiles": [
                {"path": str(stdout), "sha256": hashlib.sha256(stdout.read_bytes()).hexdigest(), "error": None},
                {"path": str(stderr), "sha256": hashlib.sha256(stderr.read_bytes()).hexdigest(), "error": None},
            ],
            "applicationLog": {
                "path": str(application_log),
                "sha256": hashlib.sha256(application_log.read_bytes()).hexdigest(),
                "error": None,
            },
            "runtimeModules": [
                {"name": "Qt6Core.dll", "path": str(self.install / "bin" / "Qt6Core.dll")},
                {"name": "Qt6Gui.dll", "path": str(self.install / "bin" / "Qt6Gui.dll")},
                {"name": "Qt6Widgets.dll", "path": str(self.install / "bin" / "Qt6Widgets.dll")},
                {"name": "qwindows.dll", "path": str(self.install / "bin" / "platforms" / "qwindows.dll")},
            ],
        }
        if mutate is not None:
            mutate(receipt)
        path = self.root / "runtime-evidence" / "runtime-receipt.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(receipt), encoding="utf-8")
        return path

    @staticmethod
    def checks_by_id(report):
        return {check["id"]: check for check in report["checks"]}


class InventoryTests(InventoryFixture):
    def test_dumpbin_parser_preserves_spaced_and_unicode_names_and_fails_closed(self):
        output = ("Dump of file synthetic.exe\n\nFile Type: EXECUTABLE IMAGE\n\n"
                  "  Image has the following dependencies:\n\n"
                  "  KERNEL32.dll\n  libomp custom.dll\n  Módulo especial.dll\n\n"
                  "  Summary\n        1000 .text\n")
        self.assertEqual(
            ["KERNEL32.dll", "libomp custom.dll", "Módulo especial.dll"],
            inventory_v43.parse_dumpbin_dependents(output),
        )

        malformed_entry = output.replace("  libomp custom.dll\n", "  libomp custom.dll\n  not a dependency\n")
        with self.assertRaisesRegex(inventory_v43.AuditError, "Unrecognized entry"):
            inventory_v43.parse_dumpbin_dependents(malformed_entry)
        with self.assertRaisesRegex(inventory_v43.AuditError, "Summary terminator"):
            inventory_v43.parse_dumpbin_dependents(output.replace("  Summary\n        1000 .text\n", ""))
        with self.assertRaisesRegex(inventory_v43.AuditError, "section header"):
            inventory_v43.parse_dumpbin_dependents("Dump of file synthetic.exe\n")

    def test_dumpbin_parser_accepts_optional_delay_load_section_and_rejects_malformed_entries(self):
        output = self.dumpbin_result(["KERNEL32.dll"], ["User Interface.dll", "Módulo diferido.dll"])
        self.assertEqual(["KERNEL32.dll", "Módulo diferido.dll", "User Interface.dll"],
                         inventory_v43.parse_dumpbin_dependents(output.stdout))

        malformed_entry = output.stdout.replace(
            "  User Interface.dll\n", "  User Interface.dll\n  unexpected delay entry\n")
        with self.assertRaisesRegex(inventory_v43.AuditError, "Unrecognized entry"):
            inventory_v43.parse_dumpbin_dependents(malformed_entry)

        empty_delay_section = self.dumpbin_result(["KERNEL32.dll"], []).stdout
        with self.assertRaisesRegex(inventory_v43.AuditError, "delay-load dependency section contains no DLL"):
            inventory_v43.parse_dumpbin_dependents(empty_delay_section)

    def test_tree_entry_cap_is_enforced_before_materializing_unbounded_children(self):
        class EndlessScan:
            def __init__(self):
                self.yielded = 0

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def __iter__(self):
                return self

            def __next__(self):
                self.yielded += 1
                return object()

        scan = EndlessScan()
        with mock.patch.object(inventory_v43.os, "scandir", return_value=scan):
            with self.assertRaisesRegex(inventory_v43.AuditError, "bounded entry count 1"):
                inventory_v43.scan_tree(self.install, "install", max_files=1,
                                         max_entries=1, max_total_bytes=1024)
        self.assertEqual(2, scan.yielded)

    def test_finalization_scan_also_enforces_install_file_cap(self):
        root = self.root / "finalization-file-cap"
        root.mkdir()
        (root / "one.bin").write_bytes(b"1")
        (root / "two.bin").write_bytes(b"2")
        with self.assertRaisesRegex(inventory_v43.AuditError, "bounded file count 1"):
            inventory_v43.scan_tree(root, "install finalization", max_files=1,
                                    max_entries=10, max_total_bytes=1024)

    def test_complete_synthetic_inventory_passes_with_explicit_limits(self):
        report = self.run_audit()
        self.assertEqual("PASS", report["inventoryStatus"])
        self.assertEqual("INCOMPLETE_LINK_MAP_NOT_SUPPLIED", report["releaseQualificationStatus"])
        self.assertEqual("UNAVAILABLE_NOT_SUPPLIED", report["linkMapProof"]["status"])
        self.assertEqual("NOT_MEASURED", report["runtimePluginLoadProof"]["status"])
        self.assertEqual("NOT_ASSESSED", report["legalReview"]["status"])
        self.assertEqual(2, report["schemaVersion"])
        self.assertEqual(len(report["files"]), report["fileCount"])
        self.assertEqual(list(report["directories"]), sorted(
            report["directories"], key=lambda value: (value.casefold(), value)))
        self.assertIn("bin", report["directories"])
        self.assertNotIn("", report["directories"])
        self.assertEqual(2, len(report["dllFiles"]))
        self.assertEqual(71, self.checks_by_id(report)["qtAttributionPages"]["details"]["expectedCount"])
        self.assertEqual(9, self.checks_by_id(report)["projectExecutablesAtCmakeInstallPaths"]["details"][
            "expectedProjectExecutableCount"])
        self.assertEqual("PASS", self.checks_by_id(report)["projectExecutablesAtCmakeInstallPaths"]["status"])
        self.assertEqual(9, self.checks_by_id(report)["projectExecutablesMatchReleasePeers"]["details"][
            "projectExecutableCount"])
        self.assertEqual("PASS", self.checks_by_id(report)["agentOffOperationsDocumentationAbsent"]["status"])
        for row in report["executableProvenance"]:
            self.assertEqual("PROJECT_RELEASE_BUILD", row["kind"])
            self.assertTrue(row["dumpbinIntegrity"]["preDumpbinMatchesInventory"])
            self.assertTrue(row["dumpbinIntegrity"]["postDumpbinMatchesInventory"])

    def test_matching_release_link_map_is_bound_and_left_unreviewed(self):
        link_map = self.write_matching_link_map()
        report = self.run_audit(link_map_value=link_map)

        self.assertEqual("PASS", report["inventoryStatus"])
        self.assertEqual("INCOMPLETE_LINK_MAP_REVIEW_REQUIRED", report["releaseQualificationStatus"])
        self.assertEqual("SUPPLIED_UNREVIEWED", report["linkMapProof"]["status"])
        self.assertEqual(hashlib.sha256(link_map.read_bytes()).hexdigest(), report["linkMapProof"]["sha256"])
        self.assertEqual(report["linkMapProof"]["buildExecutableSha256"],
                         report["linkMapProof"]["installedExecutableSha256"])
        self.assertTrue(report["linkMapProof"]["tetGenArchiveObjectPresent"])

    def test_link_map_outside_release_peer_or_without_object_markers_is_rejected(self):
        external_map = self.root / "other.map"
        external_map.write_bytes(b"not the Release peer map")
        with self.assertRaisesRegex(inventory_v43.AuditError, "inside the selected build root"):
            self.run_audit(link_map_value=external_map)

        link_map = self.write_matching_link_map()
        link_map.write_bytes(b" fea\r\n Timestamp is 1\r\n Preferred load address is 1\r\n Publics by Value\r\n Static symbols\r\n")
        with self.assertRaisesRegex(inventory_v43.AuditError, "lacks expected object provenance"):
            self.run_audit(link_map_value=link_map)

    def test_link_map_size_is_bounded_before_full_scan(self):
        link_map = self.write_matching_link_map()
        with mock.patch.object(inventory_v43, "MAX_LINK_MAP_BYTES", 64):
            with self.assertRaisesRegex(inventory_v43.AuditError, "exceeds the bounded size"):
                self.run_audit(link_map_value=link_map)

    def test_installed_gui_receipt_is_bound_to_exe_and_reports_only_qwindows(self):
        receipt = self.write_passing_gui_receipt()
        report = self.run_audit(gui_runtime_receipt_value=receipt)

        proof = report["runtimePluginLoadProof"]
        self.assertEqual("UNSIGNED_RECEIPT_QWINDOWS_ONLY", proof["status"])
        self.assertEqual("UNSIGNED_LOCAL_HARNESS_RECEIPT", proof["evidenceTrust"])
        self.assertEqual("REQUIRES_INDEPENDENT_REVIEW_OF_HARNESS_AND_RAW_LOGS",
                         proof["executionAcceptance"])
        self.assertEqual(hashlib.sha256(self.gui_harness_script.read_bytes()).hexdigest(),
                         proof["harnessScriptSha256"])
        self.assertEqual({"profile/application-data/log/application.log", "stderr.txt", "stdout.txt"},
                         {row["path"] for row in proof["rawEvidenceFiles"] + [proof["applicationLog"]]})
        self.assertEqual("qwindows.dll", proof["verifiedPlatformPlugin"])
        self.assertEqual(["Qt PDF/PDFium", "Qt Multimedia/FFmpeg"], proof["notEvaluatedPlugins"])
        self.assertEqual({"bin/Qt6Core.dll", "bin/Qt6Gui.dll", "bin/Qt6Widgets.dll", "bin/platforms/qwindows.dll"},
                         {row["path"] for row in proof["observedModules"]})

    def test_installed_gui_receipt_with_wrong_exe_hash_is_rejected(self):
        receipt = self.write_passing_gui_receipt(
            lambda value: value.update({"executableSha256": "0" * 64}))
        with self.assertRaisesRegex(inventory_v43.AuditError, "does not match the inventoried fea.exe"):
            self.run_audit(gui_runtime_receipt_value=receipt)

    def test_installed_gui_receipt_requires_the_matching_harness_script(self):
        receipt = self.write_passing_gui_receipt()
        with self.assertRaisesRegex(inventory_v43.AuditError, "--gui-harness-script is required"):
            self.run_audit(gui_runtime_receipt_value=receipt, gui_harness_script_value=None)

        alternate = self.root / "other-harness.ps1"
        alternate.write_bytes(self.gui_harness_script.read_bytes())
        with self.assertRaisesRegex(inventory_v43.AuditError, "names a different harness script"):
            self.run_audit(gui_runtime_receipt_value=receipt, gui_harness_script_value=alternate)

    def test_installed_gui_receipt_harness_and_log_hashes_are_rechecked(self):
        receipt = self.write_passing_gui_receipt(
            lambda value: value.update({"harnessScriptSha256": "0" * 64}))
        with self.assertRaisesRegex(inventory_v43.AuditError, "harness hash does not match"):
            self.run_audit(gui_runtime_receipt_value=receipt)

        receipt = self.write_passing_gui_receipt()
        stdout = self.root / "runtime-evidence" / "stdout.txt"
        stdout.write_bytes(b"changed after receipt capture\n")
        with self.assertRaisesRegex(inventory_v43.AuditError, "harness output hash does not match"):
            self.run_audit(gui_runtime_receipt_value=receipt)

    def test_installed_gui_receipt_binds_module_name_and_expected_package_path(self):
        mismatched_name = self.write_passing_gui_receipt(
            lambda value: value["runtimeModules"][0].update(
                {"path": str(self.install / "bin" / "Qt6Gui.dll")}))
        with self.assertRaisesRegex(inventory_v43.AuditError, "name does not match its observed path"):
            self.run_audit(gui_runtime_receipt_value=mismatched_name)

        alternate_core = self.install / "extra" / "Qt6Core.dll"
        alternate_core.parent.mkdir(parents=True)
        alternate_core.write_bytes(b"alternate synthetic Qt Core DLL")
        wrong_package_path = self.write_passing_gui_receipt(
            lambda value: value["runtimeModules"][0].update({"path": str(alternate_core)}))
        with self.assertRaisesRegex(inventory_v43.AuditError, "expected package paths"):
            self.run_audit(gui_runtime_receipt_value=wrong_package_path)

    def test_initial_inventory_records_an_empty_directory(self):
        empty_directory = self.install / "share" / "range-workflow" / "examples" / "__pycache__"
        empty_directory.mkdir(parents=True)

        report = self.run_audit()

        self.assertEqual("PASS", report["inventoryStatus"])
        self.assertIn("share/range-workflow/examples/__pycache__", report["directories"])
        self.assertNotIn("share/range-workflow/examples/__pycache__", {
            row["path"] for row in report["files"]
        })

    def test_missing_release_peer_fails_closed(self):
        (self.release / "fea.exe").unlink()
        report = self.run_audit()
        checks = self.checks_by_id(report)
        self.assertEqual("FAIL", report["inventoryStatus"])
        self.assertEqual("FAIL", checks["projectExecutablesMatchReleasePeers"]["status"])
        self.assertIn("bin/fea.exe", checks["projectExecutablesMatchReleasePeers"]["details"]["failures"])

    def test_qtpdf_third_party_attribution_pages_are_frozen(self):
        expected = {
            "qtpdf/qtpdf-3rdparty-abseil.html",
            "qtpdf/qtpdf-3rdparty-fast-float.html",
            "qtpdf/qtpdf-3rdparty-freetype.html",
            "qtpdf/qtpdf-3rdparty-icu.html",
            "qtpdf/qtpdf-3rdparty-libjpeg-turbo.html",
            "qtpdf/qtpdf-3rdparty-libpng.html",
            "qtpdf/qtpdf-3rdparty-pdfium.html",
            "qtpdf/qtpdf-3rdparty-the-chromium-project.html",
            "qtpdf/qtpdf-3rdparty-zlib.html",
        }
        self.assertTrue(expected.issubset(set(inventory_v43.QT_ATTRIBUTION_PAGES)))
        self.assertEqual(71, len(inventory_v43.QT_ATTRIBUTION_PAGES))

    def test_each_cmake_project_executable_is_required_at_its_expected_path(self):
        for executable_name, expected_path in sorted(
                inventory_v43.PROJECT_EXECUTABLE_EXPECTED_PATH_BY_BASENAME.items()):
            with self.subTest(executable=executable_name):
                target = self.install.joinpath(*expected_path.split("/"))
                content = target.read_bytes()
                target.unlink()
                report = self.run_audit()
                checks = self.checks_by_id(report)
                self.assertEqual("FAIL", checks["projectExecutablesAtCmakeInstallPaths"]["status"])
                failures = checks["projectExecutablesAtCmakeInstallPaths"]["details"]["failures"]
                self.assertIn(executable_name, [row["executable"] for row in failures])
                target.write_bytes(content)

    def test_supplier_mapping_cannot_override_missing_project_release_peer(self):
        for executable in ("fea.exe", "fea-solver.exe"):
            with self.subTest(executable=executable):
                build_peer = self.release / executable
                installed = self.install / "bin" / executable
                build_peer.unlink()
                source = self.root / "supplier" / executable
                source.parent.mkdir(exist_ok=True)
                source_bytes = installed.read_bytes()
                source.write_bytes(source_bytes)
                expected_sha = hashlib.sha256(source_bytes).hexdigest()
                mapping = [("bin/" + executable, source, expected_sha)]

                with self.assertRaisesRegex(inventory_v43.AuditError,
                                            "cannot override a reserved project executable basename"):
                    self.run_audit(supplier_exe_mappings=mapping)

                build_peer.write_bytes(source_bytes)
                source.unlink()

    def test_relocated_fea_and_solver_fail_exact_path_and_missing_peer_checks(self):
        for executable in ("fea.exe", "fea-solver.exe"):
            with self.subTest(executable=executable):
                expected_path = self.install / "bin" / executable
                relocated_path = self.install / "tools" / executable
                relocated_path.parent.mkdir(exist_ok=True)
                content = expected_path.read_bytes()
                expected_path.unlink()
                relocated_path.write_bytes(content)
                build_peer = self.release / executable

                with_release_peer = self.run_audit()
                checks_with_peer = self.checks_by_id(with_release_peer)
                self.assertEqual("FAIL", checks_with_peer["projectExecutablesAtCmakeInstallPaths"]["status"])
                self.assertEqual("PASS", checks_with_peer["projectExecutablesMatchReleasePeers"]["status"])

                build_peer.unlink()
                source = self.root / "supplier" / executable
                source.parent.mkdir(exist_ok=True)
                source.write_bytes(content)
                expected_sha = hashlib.sha256(content).hexdigest()

                with self.assertRaisesRegex(inventory_v43.AuditError,
                                            "cannot override a reserved project executable basename"):
                    self.run_audit(supplier_exe_mappings=[("tools/" + executable, source, expected_sha)])

                report = self.run_audit()
                checks = self.checks_by_id(report)
                path_failures = checks["projectExecutablesAtCmakeInstallPaths"]["details"]["failures"]
                failure = next(row for row in path_failures if row["executable"] == executable)
                self.assertEqual("bin/" + executable, failure["expectedInstallPath"])
                self.assertEqual(["tools/" + executable], failure["installedPaths"])
                self.assertEqual("FAIL", checks["projectExecutablesAtCmakeInstallPaths"]["status"])
                self.assertIn("tools/" + executable,
                              checks["projectExecutablesMatchReleasePeers"]["details"]["failures"])

                expected_path.write_bytes(content)
                relocated_path.unlink()
                build_peer.write_bytes(content)
                source.unlink()

    def test_supplier_mapping_is_forbidden_for_all_cmake_installed_project_targets(self):
        source = self.root / "supplier" / "project-target.exe"
        source.parent.mkdir()
        source.write_bytes(b"synthetic supplier mapping")
        expected_sha = hashlib.sha256(source.read_bytes()).hexdigest()
        expected_names = frozenset(("fea.exe", "fea-solver.exe", "range-control-velocity.exe",
                                    "range-mesh-worker.exe", "range-model-command.exe", "range-project.exe",
                                    "range-native-results.exe", "range-workflow-native.exe",
                                    "range-workflow-isolation-launcher.exe"))
        self.assertEqual(expected_names, inventory_v43.PROJECT_EXECUTABLE_BASENAMES)

        for executable in sorted(inventory_v43.PROJECT_EXECUTABLE_BASENAMES):
            with self.subTest(executable=executable):
                with self.assertRaisesRegex(inventory_v43.AuditError,
                                            "cannot override a reserved project executable basename"):
                    self.run_audit(supplier_exe_mappings=[("tools/" + executable, source, expected_sha)])

    def test_project_supplier_mapping_rejects_windows_path_and_name_aliases(self):
        source = self.root / "supplier" / "project-target.exe"
        source.parent.mkdir()
        source.write_bytes(b"synthetic supplier mapping")
        expected_sha = hashlib.sha256(source.read_bytes()).hexdigest()

        alias_paths = (
            "tools/FEA.EXE",
            r"tools\FeA.ExE",
            "tools/fea.exe.",
            "tools/fea.exe ",
            "tools/fea.exe.exe",
            "tools/fea.exe::$DATA",
            "bin/../tools/fea.exe",
            r"\\?\C:\install\tools\fea.exe",
        )
        for installed_path in alias_paths:
            with self.subTest(installed_path=installed_path):
                with self.assertRaises(inventory_v43.AuditError):
                    self.run_audit(supplier_exe_mappings=[(installed_path, source, expected_sha)])

    def test_debug_executable_cannot_substitute_for_release_peer(self):
        (self.release / "fea.exe").unlink()
        debug = self.build / "Debug" / "fea.exe"
        debug.parent.mkdir(parents=True)
        debug.write_bytes((self.install / "bin" / "fea.exe").read_bytes())
        report = self.run_audit()
        checks = self.checks_by_id(report)
        self.assertEqual("FAIL", checks["projectExecutablesMatchReleasePeers"]["status"])
        fea = next(row for row in checks["projectExecutablesMatchReleasePeers"]["details"]["executables"]
                   if row["installedPath"] == "bin/fea.exe")
        self.assertEqual([], fea["releaseBuildCandidates"])

    def test_libomp_file_and_import_are_both_reported_as_failures(self):
        (self.install / "bin" / "libomp140.x86_64.dll").write_bytes(b"forbidden runtime")

        def with_libomp(executable):
            dependencies = list(self.dependencies) + (["libomp140.x86_64.dll"]
                                                       if executable.name.casefold() == "fea.exe" else [])
            return self.dumpbin_result(dependencies)

        report = self.run_audit(dumpbin_runner=with_libomp)
        checks = self.checks_by_id(report)
        self.assertEqual("FAIL", checks["noLibompFile"]["status"])
        self.assertEqual("FAIL", checks["noLibompImport"]["status"])

    def test_libomp_import_with_spaces_is_preserved_and_rejected(self):
        def with_spaced_libomp(executable):
            dependencies = list(self.dependencies)
            if executable.name.casefold() == "fea.exe":
                dependencies.append("libomp custom.dll")
            return self.dumpbin_result(dependencies)

        report = self.run_audit(dumpbin_runner=with_spaced_libomp)
        check = self.checks_by_id(report)["noLibompImport"]
        self.assertEqual("FAIL", check["status"])
        self.assertEqual([{"executable": "bin/fea.exe", "name": "libomp custom.dll"}],
                         check["details"]["imports"])

    def test_safe_delay_load_dependency_is_reported_without_failing_inventory(self):
        def with_delay_dependency(executable):
            delayed = ["USER32.dll"] if executable.name.casefold() == "fea.exe" else None
            return self.dumpbin_result(self.dependencies, delayed)

        report = self.run_audit(dumpbin_runner=with_delay_dependency)
        self.assertEqual("PASS", report["inventoryStatus"])
        fea = next(row for row in report["executableProvenance"]
                   if row["installedPath"] == "bin/fea.exe")
        self.assertIn("USER32.dll", fea["peDependents"])
        self.assertEqual("PASS", self.checks_by_id(report)["noLibompImport"]["status"])

    def test_delay_load_libomp_import_is_rejected(self):
        def with_delayed_libomp(executable):
            delayed = ["libomp delay runtime.dll"] if executable.name.casefold() == "fea.exe" else None
            return self.dumpbin_result(self.dependencies, delayed)

        report = self.run_audit(dumpbin_runner=with_delayed_libomp)
        check = self.checks_by_id(report)["noLibompImport"]
        self.assertEqual("FAIL", check["status"])
        self.assertEqual([{"executable": "bin/fea.exe", "name": "libomp delay runtime.dll"}],
                         check["details"]["imports"])

    def test_final_rescan_detects_persistent_exe_dll_mutations_and_added_libomp(self):
        fea = self.install / "bin" / "fea.exe"
        qt_dll = self.install / "bin" / "Qt6Core.dll"
        libomp = self.install / "bin" / "libomp custom.dll"

        def mutate_after_earlier_dumpbin(executable):
            if executable.name.casefold() == "range-workflow-native.exe":
                fea.write_bytes(b"persistent post-dumpbin executable mutation")
                qt_dll.write_bytes(b"persistent post-inventory Qt DLL mutation")
                libomp.write_bytes(b"added after inventory")
            return self.dumpbin_result(self.dependencies)

        report = self.run_audit(dumpbin_runner=mutate_after_earlier_dumpbin)
        checks = self.checks_by_id(report)
        tree_check = checks["installTreeStableAfterDependencyScan"]
        self.assertEqual("FAIL", tree_check["status"])
        mutation_paths = {row["path"] for row in tree_check["details"]["mutatedFiles"]}
        self.assertIn("bin/fea.exe", mutation_paths)
        self.assertIn("bin/Qt6Core.dll", mutation_paths)
        self.assertIn("bin/libomp custom.dll", tree_check["details"]["addedPaths"])
        self.assertEqual("FAIL", checks["noLibompFile"]["status"])
        self.assertIn("bin/libomp custom.dll", checks["noLibompFile"]["details"]["paths"])
        self.assertEqual("FAIL", report["inventoryStatus"])

    def test_final_rescan_detects_file_removed_during_later_dumpbin(self):
        notice = self.install / "doc" / "licenses" / "Qt-sdk" / "LICENSE"

        def remove_after_earlier_dumpbin(executable):
            if executable.name.casefold() == "range-workflow-native.exe":
                notice.unlink()
            return self.dumpbin_result(self.dependencies)

        report = self.run_audit(dumpbin_runner=remove_after_earlier_dumpbin)
        check = self.checks_by_id(report)["installTreeStableAfterDependencyScan"]
        self.assertEqual("FAIL", check["status"])
        self.assertIn("doc/licenses/Qt-sdk/LICENSE", check["details"]["removedPaths"])

    def test_final_rescan_rejects_an_empty_directory_added_after_initial_scan(self):
        added_directory = self.install / "share" / "late-empty-directory"

        def add_empty_directory_after_initial_scan(executable):
            if executable.name.casefold() == "range-workflow-native.exe":
                added_directory.mkdir(parents=True)
            return self.dumpbin_result(self.dependencies)

        report = self.run_audit(dumpbin_runner=add_empty_directory_after_initial_scan)
        check = self.checks_by_id(report)["installTreeStableAfterDependencyScan"]

        self.assertEqual("FAIL", check["status"])
        self.assertIn("share/late-empty-directory", check["details"]["addedDirectories"])
        self.assertEqual("FAIL", report["inventoryStatus"])

    def test_final_rescan_rejects_an_empty_directory_removed_after_initial_scan(self):
        removed_directory = self.install / "share" / "range-workflow" / "examples" / "__pycache__"
        removed_directory.mkdir(parents=True)

        def remove_empty_directory_after_initial_scan(executable):
            if executable.name.casefold() == "range-workflow-native.exe":
                removed_directory.rmdir()
            return self.dumpbin_result(self.dependencies)

        report = self.run_audit(dumpbin_runner=remove_empty_directory_after_initial_scan)
        check = self.checks_by_id(report)["installTreeStableAfterDependencyScan"]

        self.assertEqual("FAIL", check["status"])
        self.assertIn("share/range-workflow/examples/__pycache__", check["details"]["removedDirectories"])
        self.assertEqual("FAIL", report["inventoryStatus"])


    def test_missing_supplier_notice_and_attribution_page_are_named(self):
        (self.install / "doc" / "licenses" / "Qt-sdk" / "LICENSE.FDL").unlink()
        missing_page = inventory_v43.QT_ATTRIBUTION_PAGES[0]
        (self.install / "doc" / "licenses" / "Qt-sdk" / "attribution" / Path(missing_page)).unlink()
        report = self.run_audit()
        checks = self.checks_by_id(report)
        self.assertEqual("FAIL", checks["qtSupplierNoticeFiles"]["status"])
        self.assertIn("LICENSE.FDL", checks["qtSupplierNoticeFiles"]["details"]["missing"])
        self.assertEqual("FAIL", checks["qtAttributionPages"]["status"])
        self.assertIn(missing_page, checks["qtAttributionPages"]["details"]["missing"])

    def test_agent_off_checks_installed_path_names(self):
        agent_file = self.install / "bin" / "agent-provider.dll"
        agent_file.write_bytes(b"synthetic Agent marker")
        report = self.run_audit(variant="OFF")
        check = self.checks_by_id(report)["agentOffInstalledPathAbsence"]
        self.assertEqual("FAIL", check["status"])
        self.assertIn("bin/agent-provider.dll", check["details"]["matchingArtifactPaths"])

    def test_agent_off_records_docs_separately_and_rejects_operations_doc(self):
        operations_doc = self.install / "doc" / "AGENT_OPERATIONS.md"
        extra_doc = self.install / "doc" / "agent-release-notes.md"
        operations_doc.parent.mkdir(parents=True, exist_ok=True)
        operations_doc.write_text("synthetic agent operations documentation\n", encoding="utf-8")
        extra_doc.write_text("synthetic release notes\n", encoding="utf-8")

        report = self.run_audit(variant="OFF")
        checks = self.checks_by_id(report)
        artifact_check = checks["agentOffInstalledPathAbsence"]
        self.assertEqual("PASS", artifact_check["status"])
        self.assertEqual({"doc/AGENT_OPERATIONS.md", "doc/agent-release-notes.md"},
                         set(artifact_check["details"]["matchingDocumentationPaths"]))
        self.assertEqual("FAIL", checks["agentOffOperationsDocumentationAbsent"]["status"])

        operations_doc.unlink()
        report = self.run_audit(variant="OFF")
        checks = self.checks_by_id(report)
        self.assertEqual("PASS", checks["agentOffInstalledPathAbsence"]["status"])
        self.assertEqual("PASS", checks["agentOffOperationsDocumentationAbsent"]["status"])

        relocated_doc = self.install / "share" / "range-workflow" / "AGENT_OPERATIONS.md"
        relocated_doc.parent.mkdir(parents=True, exist_ok=True)
        relocated_doc.write_text("synthetic relocated operations documentation\n", encoding="utf-8")
        report = self.run_audit(variant="OFF")
        doc_check = self.checks_by_id(report)["agentOffOperationsDocumentationAbsent"]
        self.assertEqual("FAIL", doc_check["status"])
        self.assertEqual(["share/range-workflow/AGENT_OPERATIONS.md"], doc_check["details"]["presentPaths"])

    def test_agent_off_rejects_agent_ui_resources(self):
        resource = self.install / "share" / "range-workflow" / "agent-panel.qml"
        resource.parent.mkdir(parents=True, exist_ok=True)
        resource.write_text("Item {}\n", encoding="utf-8")
        report = self.run_audit(variant="OFF")
        check = self.checks_by_id(report)["agentOffInstalledPathAbsence"]
        self.assertEqual("FAIL", check["status"])
        self.assertIn("share/range-workflow/agent-panel.qml",
                      check["details"]["matchingArtifactPaths"])

    def test_packaged_vcomp_must_match_supplied_redist_bytes(self):
        self.redist.write_bytes(b"different redist bytes")
        report = self.run_audit()
        check = self.checks_by_id(report)["packagedVcompMatchesSuppliedRedist"]
        self.assertEqual("FAIL", check["status"])

    def test_both_required_executables_must_import_vcomp(self):
        def without_solver_vcomp(executable):
            dependencies = list(self.dependencies)
            if executable.name.casefold() == "fea-solver.exe":
                dependencies = [name for name in dependencies if name.casefold() != "vcomp140.dll"]
            return self.dumpbin_result(dependencies)

        report = self.run_audit(dumpbin_runner=without_solver_vcomp)
        check = self.checks_by_id(report)["requiredExecutablesImportVcomp140"]
        self.assertEqual("FAIL", check["status"])
        self.assertEqual("FAIL", report["inventoryStatus"])

    def test_unmapped_supplier_executable_without_release_peer_fails(self):
        openssl = self.install / "bin" / "openssl.exe"
        openssl.write_bytes(b"synthetic supplier OpenSSL PE bytes")
        report = self.run_audit()
        check = self.checks_by_id(report)["supplierExecutableSourcesMatch"]
        self.assertEqual("FAIL", check["status"])
        self.assertIn("bin/openssl.exe", check["details"]["failures"])

    def test_supplier_executable_requires_explicit_source_and_exact_sha256(self):
        source = self.root / "supplier" / "openssl.exe"
        source.parent.mkdir()
        source_bytes = b"synthetic supplier OpenSSL PE bytes"
        source.write_bytes(source_bytes)
        (self.install / "bin" / "openssl.exe").write_bytes(source_bytes)
        expected_sha = hashlib.sha256(source_bytes).hexdigest()
        mapping = [("bin/openssl.exe", source, expected_sha)]

        report = self.run_audit(supplier_exe_mappings=mapping)
        checks = self.checks_by_id(report)
        self.assertEqual("PASS", checks["supplierExecutableSourcesMatch"]["status"])
        self.assertEqual("PASS", report["inventoryStatus"])
        provenance = next(row for row in report["executableProvenance"]
                          if row["installedPath"] == "bin/openssl.exe")
        self.assertEqual("SUPPLIER", provenance["kind"])
        self.assertEqual(expected_sha, provenance["expectedSourceSha256"])
        self.assertTrue(provenance["installedBytesMatchSource"])
        with self.assertRaises(inventory_v43.AuditError):
            self.run_audit(supplier_exe_mappings=[("bin/openssl.exe",
                                                   self.install / "bin" / "openssl.exe", expected_sha)])

        wrong_hash = "0" * 64 if expected_sha != "0" * 64 else "1" * 64
        report = self.run_audit(supplier_exe_mappings=[("bin/openssl.exe", source, wrong_hash)])
        checks = self.checks_by_id(report)
        self.assertEqual("FAIL", checks["supplierExecutableSourcesMatch"]["status"])
        self.assertEqual("FAIL", report["inventoryStatus"])

        (self.install / "bin" / "openssl.exe").write_bytes(b"installed bytes differ from source")
        report = self.run_audit(supplier_exe_mappings=mapping)
        provenance = next(row for row in report["executableProvenance"]
                          if row["installedPath"] == "bin/openssl.exe")
        self.assertFalse(provenance["installedBytesMatchSource"])
        self.assertEqual("FAIL", self.checks_by_id(report)["supplierExecutableSourcesMatch"]["status"])

    def test_duplicate_and_misplaced_supplier_mappings_fail_closed(self):
        source = self.root / "supplier" / "openssl.exe"
        source.parent.mkdir()
        source_bytes = b"synthetic supplier OpenSSL PE bytes"
        source.write_bytes(source_bytes)
        (self.install / "bin" / "openssl.exe").write_bytes(source_bytes)
        expected_sha = hashlib.sha256(source_bytes).hexdigest()

        valid_mapping = ("bin/openssl.exe", source, expected_sha)
        with self.assertRaisesRegex(inventory_v43.AuditError, "Duplicate --supplier-exe mapping"):
            self.run_audit(supplier_exe_mappings=[valid_mapping, valid_mapping])

        report = self.run_audit(supplier_exe_mappings=[("bin/openssl-copy.exe", source, expected_sha)])
        check = self.checks_by_id(report)["supplierExecutableSourcesMatch"]
        self.assertEqual("FAIL", check["status"])
        self.assertEqual(["bin/openssl.exe"], check["details"]["failures"])
        self.assertEqual(["bin/openssl-copy.exe"], check["details"]["unusedMappings"])
        self.assertEqual("FAIL", report["inventoryStatus"])

    def test_executable_mutation_during_dumpbin_fails_post_hash_check(self):
        target = self.install / "bin" / "fea.exe"

        def mutate_during_dumpbin(executable):
            if executable.name.casefold() == "fea.exe":
                executable.write_bytes(b"mutated after inventory hash")
            return self.dumpbin_result(self.dependencies)

        report = self.run_audit(dumpbin_runner=mutate_during_dumpbin)
        check = self.checks_by_id(report)["installedExecutablesStableAcrossDumpbin"]
        self.assertEqual("FAIL", check["status"])
        fea = next(row for row in report["executableProvenance"] if row["installedPath"] == "bin/fea.exe")
        self.assertTrue(fea["dumpbinIntegrity"]["preDumpbinMatchesInventory"])
        self.assertFalse(fea["dumpbinIntegrity"]["postDumpbinMatchesInventory"])
        self.assertEqual("FAIL", report["inventoryStatus"])

    def test_receipt_is_new_outside_install_and_refuses_overwrite(self):
        report = self.run_audit()
        receipt = self.root / "receipt.json"
        written = inventory_v43.write_report(receipt, report, self.install)
        self.assertEqual(receipt, written)
        self.assertEqual("PASS", json.loads(receipt.read_text(encoding="utf-8"))["inventoryStatus"])
        with self.assertRaises(inventory_v43.AuditError):
            inventory_v43.write_report(receipt, report, self.install)
        with self.assertRaises(inventory_v43.AuditError):
            inventory_v43.write_report(self.install / "receipt.json", report, self.install)

    def test_dumpbin_invocation_uses_explicit_tool_and_executable_paths(self):
        expected = self.dumpbin_result(["VCOMP140.DLL"])
        with mock.patch.object(inventory_v43.subprocess, "run", return_value=expected) as run:
            result = inventory_v43.load_pe_dependencies(self.dumpbin, self.install / "bin" / "fea.exe")
        self.assertEqual(["VCOMP140.DLL"], result)
        args, kwargs = run.call_args
        self.assertEqual(str(self.dumpbin), args[0][0])
        self.assertEqual("/DEPENDENTS", args[0][1])
        self.assertEqual(str(self.install / "bin" / "fea.exe"), args[0][2])
        self.assertEqual(30, kwargs["timeout"])
        self.assertEqual(locale.getpreferredencoding(False), kwargs["encoding"])

    def test_dumpbin_output_decode_error_fails_closed(self):
        decode_error = UnicodeDecodeError("test-codec", b"\xff", 0, 1, "invalid byte")
        with mock.patch.object(inventory_v43.subprocess, "run", side_effect=decode_error):
            with self.assertRaisesRegex(inventory_v43.AuditError, "cannot be decoded"):
                inventory_v43.run_dumpbin(self.dumpbin, self.install / "bin" / "fea.exe")


if __name__ == "__main__":
    unittest.main()
