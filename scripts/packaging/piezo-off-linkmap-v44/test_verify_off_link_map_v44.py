import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

import verify_off_link_map_v44 as gate


def make_pe(timestamp):
    pe_offset = 0x80
    optional_size = 0xF0
    data = bytearray(pe_offset + 24 + optional_size)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_offset)
    data[pe_offset:pe_offset + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, pe_offset + 4,
                     0x8664, 1, timestamp, 0, 0, optional_size, 0x22)
    optional = pe_offset + 24
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<I", data, optional + 56, 0x2000)
    struct.pack_into("<I", data, optional + 60, 0x200)
    struct.pack_into("<H", data, optional + 68, 3)
    return bytes(data)


def make_map(*, timestamp="1234abcd", public_rows=None, static_rows=None,
             include_publics=True, duplicate_publics=False, trailer=True):
    public_rows = public_rows if public_rows is not None else [
        " 0001:00000000       ?requestFileUpload@RCloudClient@@ 0000000140001000 f   range-cloud-lib:rcl_cloud_client.obj",
        " 0001:00000004       ?requestFileRemove@RCloudToolAction@@ 0000000140001004 f   range-cloud-lib:rcl_cloud_tool_action.obj",
        " 0001:00000010       ?send@RHttpClient@@              0000000140001010 f   range-cloud-lib:rhttp_client.obj",
    ]
    static_rows = static_rows if static_rows is not None else [
        " 0001:00000020       ?cloudSettings@RCloudClient@@   0000000140001020     range-cloud-lib:rcl_cloud_client.obj",
    ]
    rows = [
        " fea",
        "",
        " Timestamp is {} (Wed Oct 14 11:59:26 2026)".format(timestamp),
        "",
        " Preferred load address is 0000000140000000",
        "",
        " Start         Length     Name                   Class",
        " 0001:00000000 00000100H .text                  CODE",
        "",
    ]
    if include_publics:
        rows.append("  Address         Publics by Value              Rva+Base               Lib:Object")
        rows.extend(public_rows)
        rows.append(" entry point at        0001:00000000")
    if duplicate_publics:
        rows.append("  Address         Publics by Value              Rva+Base               Lib:Object")
    rows.append(" Static symbols")
    rows.extend(static_rows)
    if trailer:
        rows.append("")
    return ("\n".join(rows) + "\n").encode("utf-8")


class LinkMapGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.build_root = self.root / "build"
        self.install_root = self.root / "install"
        (self.build_root / "Release").mkdir(parents=True)
        (self.build_root / "fea" / "Release").mkdir(parents=True)
        (self.install_root / "bin").mkdir(parents=True)
        self.build_exe = self.build_root / "Release" / "fea.exe"
        self.install_exe = self.install_root / "bin" / "fea.exe"
        self.map_path = self.build_root / "fea" / "Release" / "fea-link.map"
        self.source_proof = self.root / "source-manifest.json"
        self.report_path = self.root / "inventory-v43-off.json"
        self.timestamp = 0x1234ABCD
        self.pe = make_pe(self.timestamp)
        self.build_exe.write_bytes(self.pe)
        self.install_exe.write_bytes(self.pe)
        self.source_proof.write_text('{"sourceRevision":"external-test-fixture"}\n', encoding="utf-8")
        self.map_path.write_bytes(make_map(timestamp="{:08x}".format(self.timestamp)))
        self._write_report()

    def tearDown(self):
        self.temp.cleanup()

    def _write_report(self, *, variant="OFF", inventory_status="PASS", release_status="INCOMPLETE_LINK_MAP_NOT_SUPPLIED",
                      build_sha=None, install_sha=None, include_provenance=True, include_file=True):
        sha = hashlib.sha256(self.pe).hexdigest()
        report = {
            "schemaVersion": 2,
            "variant": variant,
            "inventoryStatus": inventory_status,
            "releaseQualificationStatus": release_status,
            "buildRoot": str(self.build_root),
            "installRoot": str(self.install_root),
            "files": [],
            "executableProvenance": [],
        }
        if include_file:
            report["files"].append({"path": "bin/fea.exe", "sha256": install_sha or sha, "sizeBytes": len(self.pe)})
        if include_provenance:
            report["executableProvenance"].append({
                "installedPath": "bin/fea.exe",
                "installedSha256": install_sha or sha,
                "installedSizeBytes": len(self.pe),
                "releaseBuildPath": "Release/fea.exe",
                "releaseBuildCandidates": ["Release/fea.exe"],
                "releaseBuildSha256": build_sha or sha,
                "releaseBuildSizeBytes": len(self.pe),
                "bytesEqualBySizeAndSha256": True,
                "kind": "PROJECT_RELEASE_BUILD",
                "status": "PASS",
            })
        self.report_path.write_text(json.dumps(report, sort_keys=True), encoding="utf-8")

    def _audit(self):
        return gate.audit_off_link_map(
            self.map_path, self.build_exe, self.install_exe,
            self.source_proof, hashlib.sha256(self.source_proof.read_bytes()).hexdigest(),
            self.report_path, hashlib.sha256(self.report_path.read_bytes()).hexdigest(),
            hashlib.sha256(self.map_path.read_bytes()).hexdigest(),
        )

    def test_generic_cloud_and_http_members_are_allowed(self):
        receipt = self._audit()
        self.assertEqual("PASS", receipt["linkMapGateStatus"])
        self.assertFalse(receipt["goPermitted"])
        self.assertEqual("INCOMPLETE_LINK_MAP_NOT_SUPPLIED",
                         receipt["pinnedInventoryReport"]["releaseQualificationStatus"])
        self.assertGreater(receipt["map"]["publicSymbolCount"], 0)
        self.assertGreater(receipt["map"]["staticSymbolCount"], 0)

    def test_legacy_cloud_ai_widget_archive_member_fails(self):
        self.map_path.write_bytes(make_map(public_rows=[
            " 0001:00000000       ?show@RCloudAiQueryWidget@@ 0000000140001000 f   range-gui-lib:rgl_cloud_ai_query_widget.obj",
        ]))
        self._write_report()
        receipt = self._audit()
        self.assertEqual("FAIL", receipt["linkMapGateStatus"])
        self.assertTrue(any("rgl_cloud_ai_query_widget.obj" in row["provenance"]
                            for row in receipt["map"]["forbiddenRows"]))

    def test_legacy_client_query_and_request_response_symbols_fail(self):
        for symbol, member in (
            ("?requestAIQuery@RCloudClient@@", "range-cloud-lib:rcl_cloud_client.obj"),
            ("?requestAIQueryResult@RCloudToolAction@@", "range-cloud-lib:rcl_cloud_tool_action.obj"),
            ("?key@AIQuery@@", "range-cloud-lib:rcl_ai_query.obj"),
            ("?getId@RCloudAIQueryResponse@@", "range-cloud-lib:rcl_cloud_ai_query_response.obj"),
        ):
            with self.subTest(symbol=symbol):
                self.map_path.write_bytes(make_map(public_rows=[
                    " 0001:00000000 {} 0000000140001000 f   {}".format(symbol, member),
                ]))
                self._write_report()
                self.assertEqual("FAIL", self._audit()["linkMapGateStatus"])

    def test_new_agent_archive_or_symbol_fails(self):
        for row in (
            " 0001:00000000       ?run@AgentTaskRunner@@ 0000000140001000 f   range-ai-lib:agent_task_runner.obj",
            " 0001:00000000       ?create@AgentWidget@@ 0000000140001000 f   range-gui-lib:rgl_agent_widget.obj",
            " 0001:00000000       ?show@RAIChatWidget@@ 0000000140001000 f   range-gui-lib:rgl_ai_chat_widget.obj",
        ):
            with self.subTest(row=row):
                self.map_path.write_bytes(make_map(public_rows=[row]))
                self._write_report()
                self.assertEqual("FAIL", self._audit()["linkMapGateStatus"])

    def test_agent_row_between_entry_point_and_static_symbols_is_rejected(self):
        map_bytes = make_map()
        marker = b" Static symbols"
        injected = (b" 0001:00000024       ?run@AgentTaskRunner@@ 0000000140001024 f   "
                    b"fea:agent_task_runner.obj\r\n")
        self.assertIn(marker, map_bytes)
        self.map_path.write_bytes(map_bytes.replace(marker, injected + marker, 1))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "between MSVC entry point and Static symbols"):
            self._audit()

    def test_unrecognized_preamble_row_is_rejected(self):
        map_bytes = make_map()
        marker = b" Timestamp is "
        injected = b" 0001:00000024       ?run@AgentTaskRunner@@ 0000000140001024 f   fea:agent_task_runner.obj\r\n"
        self.assertIn(marker, map_bytes)
        self.map_path.write_bytes(map_bytes.replace(marker, injected + marker, 1))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "preamble row"):
            self._audit()

    def test_publics_is_required(self):
        self.map_path.write_bytes(make_map(include_publics=False))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "Publics"):
            self._audit()

    def test_duplicate_publics_header_is_rejected(self):
        self.map_path.write_bytes(make_map(duplicate_publics=True))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "exactly one"):
            self._audit()

    def test_truncated_or_unrecognized_symbol_rows_are_rejected(self):
        self.map_path.write_bytes(make_map(public_rows=[" 0001:00000000 missing-columns"]))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "Unrecognized Publics row"):
            self._audit()

    def test_structurally_valid_partial_map_is_rejected_by_external_pin(self):
        original_map_pin = hashlib.sha256(self.map_path.read_bytes()).hexdigest()
        self.map_path.write_bytes(make_map(static_rows=[
            " 0001:00000020       ?cloudSettings@RCloudClient@@   0000000140001020     range-cloud-lib:rcl_cloud_client.obj",
            " 0001:00000030       ?session@RHttpClient@@          0000000140001030     range-cloud-lib:rhttp_client.obj",
        ]))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "map SHA-256"):
            gate.audit_off_link_map(
                self.map_path, self.build_exe, self.install_exe,
                self.source_proof, hashlib.sha256(self.source_proof.read_bytes()).hexdigest(),
                self.report_path, hashlib.sha256(self.report_path.read_bytes()).hexdigest(),
                original_map_pin,
            )

    def test_timestamp_must_match_build_and_install_pe(self):
        self.map_path.write_bytes(make_map(timestamp="deadbeef"))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "timestamp"):
            self._audit()

    def test_install_pe_must_match_build_bytes(self):
        self.install_exe.write_bytes(make_pe(self.timestamp + 1))
        self._write_report()
        with self.assertRaisesRegex(gate.AuditError, "exactly match"):
            self._audit()

    def test_all_three_external_pins_are_verified(self):
        pins = (
            self.source_proof, hashlib.sha256(self.source_proof.read_bytes()).hexdigest(),
            self.report_path, hashlib.sha256(self.report_path.read_bytes()).hexdigest(),
            hashlib.sha256(self.map_path.read_bytes()).hexdigest(),
        )
        for bad_pin, expected_message in (("0" * 64, "source proof SHA-256"),):
            with self.subTest(pin=expected_message):
                with self.assertRaisesRegex(gate.AuditError, expected_message):
                    gate.audit_off_link_map(
                        self.map_path, self.build_exe, self.install_exe,
                        pins[0], bad_pin, pins[2], pins[3], pins[4],
                    )
        with self.assertRaisesRegex(gate.AuditError, "inventory report SHA-256"):
            gate.audit_off_link_map(
                self.map_path, self.build_exe, self.install_exe,
                pins[0], pins[1], pins[2], "0" * 64, pins[4],
            )
        with self.assertRaisesRegex(gate.AuditError, "map SHA-256"):
            gate.audit_off_link_map(
                self.map_path, self.build_exe, self.install_exe,
                pins[0], pins[1], pins[2], pins[3], "0" * 64,
            )

    def test_report_variant_status_and_exe_provenance_are_enforced(self):
        self._write_report(variant="ON")
        with self.assertRaisesRegex(gate.AuditError, "variant OFF"):
            self._audit()
        self._write_report(release_status="GO")
        with self.assertRaisesRegex(gate.AuditError, "non-PASS/non-GO"):
            self._audit()
        self._write_report()
        self._write_report(build_sha="0" * 64)
        with self.assertRaisesRegex(gate.AuditError, "provenance"):
            self._audit()

    def test_wrong_map_basename_is_rejected(self):
        other = self.map_path.with_name("fea.map")
        other.write_bytes(self.map_path.read_bytes())
        with self.assertRaisesRegex(gate.AuditError, "basename"):
            gate.audit_off_link_map(
                other, self.build_exe, self.install_exe,
                self.source_proof, hashlib.sha256(self.source_proof.read_bytes()).hexdigest(),
                self.report_path, hashlib.sha256(self.report_path.read_bytes()).hexdigest(),
                hashlib.sha256(other.read_bytes()).hexdigest(),
            )

    def test_receipt_writer_refuses_to_overwrite(self):
        output = self.root / "receipt.json"
        output.write_text("old receipt", encoding="utf-8")
        with self.assertRaisesRegex(gate.AuditError, "Refusing to overwrite"):
            gate.write_receipt(output, {"linkMapGateStatus": "PASS"})
        self.assertEqual("old receipt", output.read_text(encoding="utf-8"))

    def test_receipt_writer_keeps_output_outside_audited_tree(self):
        output = self.install_root / "bin" / "new-receipt.json"
        with self.assertRaisesRegex(gate.AuditError, "outside the audited"):
            gate.write_receipt(output, {"linkMapGateStatus": "FAIL"}, protected_roots=(self.install_root,))
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
