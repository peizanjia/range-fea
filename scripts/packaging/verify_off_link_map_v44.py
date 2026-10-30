#!/usr/bin/env python3
"""Fail-closed Agent-OFF MSVC Release link-map gate for fea.exe.

The receipt is intentionally independent of inventory_v43.py.  A PASS means
only that the pinned MSVC map has a complete, recognized Publics/Static
symbols body, contains no configured Agent/legacy Cloud AI archive members or
symbols, and is bound to byte-identical build/install PE images whose COFF
timestamp and report provenance agree.  It is not a release GO decision.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import sys


MAX_MAP_BYTES = 512 * 1024 * 1024
MAX_SOURCE_PROOF_BYTES = 256 * 1024 * 1024
MAX_REPORT_BYTES = 64 * 1024 * 1024
MAX_PE_BYTES = 2 * 1024 * 1024 * 1024
HASH_CHUNK_BYTES = 1024 * 1024
MAX_FINDING_SAMPLES = 100

SECTION_TABLE_HEADER = re.compile(r"^\s*Start\s+Length\s+Name\s+Class\s*$", re.IGNORECASE)
SECTION_ROW = re.compile(
    r"^\s*[0-9A-F]{4}:[0-9A-F]{8}\s+[0-9A-F]{8}H\s+\S+\s+\S+\s*$", re.IGNORECASE
)
PUBLICS_HEADER = re.compile(
    r"^\s*Address\s+Publics by Value\s+Rva\+Base\s+Lib:Object\s*$", re.IGNORECASE
)
STATIC_HEADER = re.compile(r"^\s*Static symbols\s*$", re.IGNORECASE)
TIMESTAMP_LINE = re.compile(r"^\s*Timestamp is ([0-9A-F]{8})(?:\s+\(.+\))?\s*$", re.IGNORECASE)
LOAD_ADDRESS_LINE = re.compile(r"^\s*Preferred load address is ([0-9A-F]{16})\s*$", re.IGNORECASE)
ENTRY_POINT_LINE = re.compile(r"^\s*entry point at\s+([0-9A-F]{4}:[0-9A-F]{8})\s*$", re.IGNORECASE)
MAP_SYMBOL_ROW = re.compile(
    r"^\s*([0-9A-F]{4}:[0-9A-F]{8})\s+(\S+)\s+([0-9A-F]{16})(?:\s+(.*?))?\s*$",
    re.IGNORECASE,
)
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
IDENTIFIER_TOKEN_RE = re.compile(r"[A-Z]+(?=[A-Z][a-z]|$)|[A-Z]?[a-z]+|[0-9]+")

# Match identity-bearing archive/object members and symbol names in parsed
# MSVC rows.  Generic Cloud, HTTP, and non-AI RCloudToolAction rows remain
# allowed.  The map's Lib:Object column is parsed separately from the symbol.
FORBIDDEN_AGENT_RE = re.compile(
    r"(?i)(?:openai|range[-_]ai(?:[-_]lib)?|rgl[_-]ai[_-]|rai[_-])"
)
FORBIDDEN_CLOUD_AI_RE = re.compile(
    r"(?i)(?:"
    r"\brgl_cloud_ai_query(?:_widget)?\b|"
    r"\brcl_ai_query\b|"
    r"\brcl_cloud_ai_query(?:_request|_response)?\b|"
    r"\bcloudaiquery\w*\b|"
    r"\baiquery\w*\b|"
    r"requestaiquery\w*|"
    r"\bRCloudAIQuery(?:Request|Response)\b|"
    r"\bRAIQuery\b|"
    r"\bAIQuery\b"
    r")"
)


class AuditError(Exception):
    """An input cannot support a trustworthy map gate."""


def _absolute_path(value):
    return Path(os.path.abspath(os.fspath(value)))


def _is_reparse_or_link(file_stat):
    flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return stat.S_ISLNK(file_stat.st_mode) or bool(getattr(file_stat, "st_file_attributes", 0) & flag)


def _validate_regular_file(value, label, max_bytes):
    path = _absolute_path(value)
    for component in reversed((path, *path.parents)):
        try:
            component_stat = component.lstat()
        except OSError as exc:
            raise AuditError("{} is unavailable: {} ({})".format(label, component, exc))
        if _is_reparse_or_link(component_stat):
            raise AuditError("{} path contains a symlink or reparse point: {}".format(label, component))
    try:
        file_stat = path.lstat()
    except OSError as exc:
        raise AuditError("{} is unavailable: {} ({})".format(label, path, exc))
    if _is_reparse_or_link(file_stat) or not stat.S_ISREG(file_stat.st_mode):
        raise AuditError("{} must be a regular non-reparse file: {}".format(label, path))
    if file_stat.st_size <= 0 or file_stat.st_size > max_bytes:
        raise AuditError("{} size is outside the allowed range 1..{} bytes: {}".format(label, max_bytes, path))
    return path.resolve(strict=True)


def _validate_directory(value, label):
    path = _absolute_path(value)
    for component in reversed((path, *path.parents)):
        try:
            component_stat = component.lstat()
        except OSError as exc:
            raise AuditError("{} is unavailable: {} ({})".format(label, component, exc))
        if _is_reparse_or_link(component_stat):
            raise AuditError("{} path contains a symlink or reparse point: {}".format(label, component))
    if not path.is_dir():
        raise AuditError("{} is not a directory: {}".format(label, path))
    return path.resolve(strict=True)


def _hash_file(path, label, max_bytes):
    try:
        before = path.lstat()
        if _is_reparse_or_link(before) or not stat.S_ISREG(before.st_mode):
            raise AuditError("{} changed into a symlink or non-regular file: {}".format(label, path))
        if before.st_size <= 0 or before.st_size > max_bytes:
            raise AuditError("{} size is outside the allowed range 1..{} bytes: {}".format(label, max_bytes, path))
        digest = hashlib.sha256()
        total = 0
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if (opened.st_size != before.st_size or
                    getattr(opened, "st_ino", 0) != getattr(before, "st_ino", 0) or
                    getattr(opened, "st_dev", 0) != getattr(before, "st_dev", 0)):
                raise AuditError("{} identity changed while opening: {}".format(label, path))
            while True:
                block = stream.read(HASH_CHUNK_BYTES)
                if not block:
                    break
                total += len(block)
                digest.update(block)
            after = os.fstat(stream.fileno())
        current = path.lstat()
        if (total != before.st_size or after.st_size != before.st_size or
                after.st_mtime_ns != before.st_mtime_ns or current.st_mtime_ns != before.st_mtime_ns):
            raise AuditError("{} changed while hashing: {}".format(label, path))
        return digest.hexdigest(), total
    except OSError as exc:
        raise AuditError("Cannot hash {} {} ({})".format(label, path, exc))


def _require_pin(expected, label):
    if not isinstance(expected, str) or not SHA256_RE.fullmatch(expected):
        raise AuditError("{} external SHA-256 pin must be exactly 64 hexadecimal characters".format(label))
    return expected.lower()


def _pinned_file(path_value, expected_sha256, label, max_bytes):
    expected = _require_pin(expected_sha256, label)
    path = _validate_regular_file(path_value, label, max_bytes)
    observed, size = _hash_file(path, label, max_bytes)
    if observed != expected:
        raise AuditError("{} SHA-256 does not match the external pin: expected {}, observed {}".format(
            label, expected, observed))
    return path, observed, size


def read_pe_identity(path_value, label):
    path = _validate_regular_file(path_value, label, MAX_PE_BYTES)
    sha256, size = _hash_file(path, label, MAX_PE_BYTES)
    try:
        with path.open("rb") as stream:
            dos = stream.read(64)
            if len(dos) != 64 or dos[:2] != b"MZ":
                raise AuditError("{} is not a PE image with a DOS header: {}".format(label, path))
            pe_offset = struct.unpack_from("<I", dos, 0x3C)[0]
            if pe_offset < 64 or pe_offset > size - 24:
                raise AuditError("{} has an invalid PE header offset: {}".format(label, path))
            stream.seek(pe_offset)
            header = stream.read(24)
            if len(header) != 24 or header[:4] != b"PE\0\0":
                raise AuditError("{} has an invalid PE signature: {}".format(label, path))
            machine, section_count, timestamp, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", header, 4)
            if machine != 0x8664:
                raise AuditError("{} must be an AMD64 PE image (machine 0x8664), got 0x{:04x}".format(
                    label, machine))
            if section_count == 0 or section_count > 96:
                raise AuditError("{} has an invalid PE section count: {}".format(label, section_count))
            optional_start = pe_offset + 24
            if optional_size < 70 or optional_start + optional_size > size:
                raise AuditError("{} has an incomplete PE optional header: {}".format(label, path))
            stream.seek(optional_start)
            optional = stream.read(optional_size)
            if len(optional) != optional_size or struct.unpack_from("<H", optional)[0] != 0x20B:
                raise AuditError("{} must be a complete PE32+ image: {}".format(label, path))
    except OSError as exc:
        raise AuditError("Cannot read PE headers for {} {} ({})".format(label, path, exc))
    if timestamp == 0:
        raise AuditError("{} PE COFF timestamp is zero and cannot bind the map: {}".format(label, path))
    final_sha256, final_size = _hash_file(path, label, MAX_PE_BYTES)
    if final_sha256 != sha256 or final_size != size:
        raise AuditError("{} changed while reading its PE header: {}".format(label, path))
    return {
        "path": str(path),
        "sizeBytes": size,
        "sha256": sha256,
        "machine": "AMD64",
        "coffTimestamp": "{:08x}".format(timestamp),
    }


def _parse_map_symbol_row(line, line_number, section):
    match = MAP_SYMBOL_ROW.fullmatch(line)
    if not match:
        raise AuditError("Unrecognized {} row at map line {}: {}".format(section, line_number, line[:240]))
    address, symbol, rva, tail = match.groups()
    tail_tokens = (tail or "").split()
    flags = []
    while tail_tokens and tail_tokens[0].casefold() in ("f", "i"):
        flags.append(tail_tokens.pop(0).casefold())
    if len(flags) != len(set(flags)):
        raise AuditError("Repeated map symbol flags at line {}".format(line_number))
    provenance = " ".join(tail_tokens)
    if not provenance:
        raise AuditError("Missing Lib:Object provenance in {} row at map line {}".format(section, line_number))
    if (provenance.casefold() not in ("<absolute>", "<linker-defined>") and not re.search(
            r"(?:^|[.:/\\])[^.:/\\]+\.(?:obj|res|dll)\b", provenance, re.IGNORECASE)):
        raise AuditError("Missing object-member identity in {} row at map line {}: {}".format(
            section, line_number, provenance[:180]))
    return {
        "line": line_number,
        "address": address.lower(),
        "symbol": symbol,
        "rvaPlusBase": rva.lower(),
        "flags": flags,
        "provenance": provenance,
    }


def _contains_agent_token(identity):
    if FORBIDDEN_AGENT_RE.search(identity):
        return True
    for component in re.split(r"[^A-Za-z0-9]+", identity):
        if any(token.casefold() == "agent" for token in IDENTIFIER_TOKEN_RE.findall(component)):
            return True
    return False


def parse_msvc_map(map_path, map_bytes):
    try:
        text = map_bytes.decode("utf-8-sig", errors="strict")
    except UnicodeDecodeError as exc:
        raise AuditError("MSVC map is not valid UTF-8 text (byte offset {})".format(exc.start))
    lines = text.splitlines()
    if not lines or lines[0].strip().casefold() != "fea":
        raise AuditError("MSVC map image title must be exactly 'fea'")

    timestamps = [(index, match.group(1).lower()) for index, line in enumerate(lines, 1)
                  if (match := TIMESTAMP_LINE.fullmatch(line))]
    if len(timestamps) != 1:
        raise AuditError("MSVC map must contain exactly one well-formed Timestamp line")
    load_addresses = [match.group(1).lower() for line in lines if (match := LOAD_ADDRESS_LINE.fullmatch(line))]
    if len(load_addresses) != 1:
        raise AuditError("MSVC map must contain exactly one Preferred load address line")

    section_headers = [index for index, line in enumerate(lines) if SECTION_TABLE_HEADER.fullmatch(line)]
    public_headers = [index for index, line in enumerate(lines) if PUBLICS_HEADER.fullmatch(line)]
    static_headers = [index for index, line in enumerate(lines) if STATIC_HEADER.fullmatch(line)]
    entry_points = [(index, match.group(1).lower()) for index, line in enumerate(lines)
                    if (match := ENTRY_POINT_LINE.fullmatch(line))]
    if len(section_headers) != 1 or len(public_headers) != 1 or len(static_headers) != 1 or len(entry_points) != 1:
        raise AuditError("MSVC map must contain exactly one section table, Publics, entry point, and Static symbols")
    section_header = section_headers[0]
    public_header = public_headers[0]
    entry_line, entry_point = entry_points[0]
    static_header = static_headers[0]
    if not (section_header < public_header < entry_line < static_header):
        raise AuditError("MSVC map sections are out of order or incomplete")
    for index in range(1, section_header):
        line = lines[index]
        if line.strip() and not (TIMESTAMP_LINE.fullmatch(line) or LOAD_ADDRESS_LINE.fullmatch(line)):
            raise AuditError("Unrecognized MSVC map preamble row at line {}: {}".format(index + 1, line[:240]))
    for index in range(entry_line + 1, static_header):
        if lines[index].strip():
            raise AuditError("Unrecognized row between MSVC entry point and Static symbols at line {}: {}".format(
                index + 1, lines[index][:240]))

    section_count = 0
    for index in range(section_header + 1, public_header):
        line = lines[index]
        if not line.strip():
            continue
        if not SECTION_ROW.fullmatch(line):
            raise AuditError("Unrecognized MSVC section-table row at line {}: {}".format(index + 1, line[:240]))
        section_count += 1
    if section_count == 0:
        raise AuditError("MSVC map section table contains no sections")

    public_rows = []
    for index in range(public_header + 1, entry_line):
        line = lines[index]
        if not line.strip():
            continue
        public_rows.append(_parse_map_symbol_row(line, index + 1, "Publics"))
    if not public_rows:
        raise AuditError("MSVC map Publics section is empty")
    if not any(row["address"] == entry_point for row in public_rows):
        raise AuditError("MSVC map entry point does not resolve to a parsed Publics row")

    static_rows = []
    for index in range(static_header + 1, len(lines)):
        line = lines[index]
        if not line.strip():
            continue
        static_rows.append(_parse_map_symbol_row(line, index + 1, "Static symbols"))
    if not static_rows:
        raise AuditError("MSVC map Static symbols section is empty or truncated")

    findings = []
    finding_count = 0
    agent_finding_count = 0
    cloud_ai_finding_count = 0
    for section_name, rows in (("Publics", public_rows), ("Static symbols", static_rows)):
        for row in rows:
            identity = row["symbol"] + " " + row["provenance"]
            reasons = []
            if _contains_agent_token(identity):
                reasons.append("Agent archive member or symbol")
                agent_finding_count += 1
            if FORBIDDEN_CLOUD_AI_RE.search(identity):
                reasons.append("legacy Cloud AI archive member or symbol")
                cloud_ai_finding_count += 1
            if reasons:
                finding_count += 1
                if len(findings) < MAX_FINDING_SAMPLES:
                    findings.append({
                        "section": section_name,
                        "line": row["line"],
                        "symbol": row["symbol"],
                        "provenance": row["provenance"],
                        "reasons": reasons,
                    })

    return {
        "imageTitle": "fea",
        "timestamp": timestamps[0][1],
        "preferredLoadAddress": load_addresses[0],
        "sectionCount": section_count,
        "publicSymbolCount": len(public_rows),
        "staticSymbolCount": len(static_rows),
        "entryPoint": entry_point,
        "lineCount": len(lines),
        "forbiddenRowCount": finding_count,
        "forbiddenAgentRowCount": agent_finding_count,
        "forbiddenCloudAiRowCount": cloud_ai_finding_count,
        "forbiddenRows": findings,
    }


def _same_path(left, right):
    return os.path.normcase(os.path.abspath(os.fspath(left))) == os.path.normcase(os.path.abspath(os.fspath(right)))


def _path_is_within(candidate, parent):
    try:
        candidate.relative_to(parent)
        return True
    except ValueError:
        return False


def _safe_report_relative(value, expected, label):
    if not isinstance(value, str) or value.replace("\\", "/") != expected:
        raise AuditError("Pinned v43 report {} must be exactly {!r}".format(label, expected))
    relative = PurePosixPath(value.replace("\\", "/"))
    if relative.is_absolute() or any(part in ("", ".", "..") for part in relative.parts):
        raise AuditError("Pinned v43 report {} is not a safe relative path".format(label))
    return relative


def _read_report(report_path, expected_sha256, expected_size):
    try:
        report_bytes = report_path.read_bytes()
        if len(report_bytes) != expected_size or hashlib.sha256(report_bytes).hexdigest() != expected_sha256:
            raise AuditError("Pinned v43 inventory report changed after its external SHA-256 check")
        report = json.loads(report_bytes.decode("utf-8", errors="strict"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AuditError("Cannot parse pinned v43 inventory report {} ({})".format(report_path, exc))
    if not isinstance(report, dict):
        raise AuditError("Pinned v43 inventory report root must be a JSON object")
    if report.get("schemaVersion") != 2 or report.get("inventoryStatus") != "PASS":
        raise AuditError("Pinned v43 report must be schemaVersion 2 with inventoryStatus PASS")
    if report.get("variant") != "OFF":
        raise AuditError("Pinned v43 report must identify variant OFF")
    # This gate is never allowed to edit or promote the v43 qualification field.
    release_status = report.get("releaseQualificationStatus")
    if not isinstance(release_status, str) or not release_status or release_status.strip().casefold() in ("pass", "go"):
        raise AuditError("Pinned v43 report must preserve a non-PASS/non-GO releaseQualificationStatus")
    return report


def audit_off_link_map(map_value, build_exe_value, install_exe_value,
                       source_proof_value, source_sha256,
                       report_value, report_sha256,
                       map_sha256):
    map_path, actual_map_sha256, map_size = _pinned_file(map_value, map_sha256, "map", MAX_MAP_BYTES)
    source_path, actual_source_sha256, source_size = _pinned_file(
        source_proof_value, source_sha256, "source proof", MAX_SOURCE_PROOF_BYTES)
    report_path, actual_report_sha256, report_size = _pinned_file(
        report_value, report_sha256, "inventory report", MAX_REPORT_BYTES)
    build_pe = read_pe_identity(build_exe_value, "Release build fea.exe")
    install_pe = read_pe_identity(install_exe_value, "OFF install fea.exe")
    input_paths = [map_path, source_path, report_path, Path(build_pe["path"]), Path(install_pe["path"])]
    normalized_inputs = [os.path.normcase(os.path.abspath(os.fspath(path))) for path in input_paths]
    if len(set(normalized_inputs)) != len(normalized_inputs):
        raise AuditError("Map, source proof, report, build PE, and installed PE must be distinct files")

    if Path(map_path).name.casefold() != "fea-link.map":
        raise AuditError("MSVC map basename must be exactly fea-link.map")
    if Path(build_pe["path"]).name.casefold() != "fea.exe" or Path(install_pe["path"]).name.casefold() != "fea.exe":
        raise AuditError("Both PE inputs must have basename fea.exe")
    if build_pe["sha256"] != install_pe["sha256"] or build_pe["sizeBytes"] != install_pe["sizeBytes"]:
        raise AuditError("OFF installed fea.exe bytes do not exactly match the Release build fea.exe")
    if build_pe["coffTimestamp"] != install_pe["coffTimestamp"]:
        raise AuditError("Build and install fea.exe PE COFF timestamps differ")

    report = _read_report(report_path, actual_report_sha256, report_size)
    if not isinstance(report.get("buildRoot"), str) or not isinstance(report.get("installRoot"), str):
        raise AuditError("Pinned v43 report is missing buildRoot or installRoot")
    build_root = _validate_directory(report["buildRoot"], "report build root")
    install_root = _validate_directory(report["installRoot"], "report install root")
    expected_build_relative = PurePosixPath("Release/fea.exe")
    expected_install_relative = PurePosixPath("bin/fea.exe")
    expected_build_exe = build_root.joinpath(*expected_build_relative.parts)
    expected_install_exe = install_root.joinpath(*expected_install_relative.parts)
    expected_map = build_root / "fea" / "Release" / "fea-link.map"
    if not _same_path(build_pe["path"], expected_build_exe):
        raise AuditError("Release build fea.exe does not match the pinned report's Release/fea.exe provenance")
    if not _same_path(install_pe["path"], expected_install_exe):
        raise AuditError("Installed fea.exe does not match the pinned report's bin/fea.exe path")
    if not _same_path(map_path, expected_map):
        raise AuditError("fea-link.map must be the report build's fea/Release/fea-link.map")

    provenance_rows = [row for row in report.get("executableProvenance", [])
                       if isinstance(row, dict) and row.get("installedPath", "").casefold() == "bin/fea.exe"]
    if len(provenance_rows) != 1:
        raise AuditError("Pinned v43 report must contain exactly one bin/fea.exe provenance row")
    provenance = provenance_rows[0]
    _safe_report_relative(provenance.get("releaseBuildPath"), "Release/fea.exe", "releaseBuildPath")
    _safe_report_relative(provenance.get("installedPath"), "bin/fea.exe", "installedPath")
    if (provenance.get("status") != "PASS" or provenance.get("kind") != "PROJECT_RELEASE_BUILD" or
            provenance.get("releaseBuildSha256") != build_pe["sha256"] or
            provenance.get("installedSha256") != install_pe["sha256"] or
            provenance.get("releaseBuildSizeBytes") != build_pe["sizeBytes"] or
            provenance.get("installedSizeBytes") != install_pe["sizeBytes"] or
            provenance.get("bytesEqualBySizeAndSha256") is not True):
        raise AuditError("Pinned v43 fea.exe provenance does not match the supplied build/install bytes")
    if provenance.get("releaseBuildCandidates") != ["Release/fea.exe"]:
        raise AuditError("Pinned v43 report must identify exactly one Release/fea.exe build candidate")
    file_rows = [row for row in report.get("files", [])
                 if isinstance(row, dict) and row.get("path", "").casefold() == "bin/fea.exe"]
    if len(file_rows) != 1 or file_rows[0].get("sha256") != install_pe["sha256"] or \
            file_rows[0].get("sizeBytes") != install_pe["sizeBytes"]:
        raise AuditError("Pinned v43 files inventory does not match the installed fea.exe bytes")

    try:
        map_bytes = map_path.read_bytes()
    except OSError as exc:
        raise AuditError("Cannot read pinned MSVC map {} ({})".format(map_path, exc))
    # Hash again after reading, so the parsed bytes are the pinned bytes.
    parsed_map_sha256 = hashlib.sha256(map_bytes).hexdigest()
    if parsed_map_sha256 != actual_map_sha256 or len(map_bytes) != map_size:
        raise AuditError("MSVC map changed after its pinned hash was computed")
    map_result = parse_msvc_map(map_path, map_bytes)
    if map_result["timestamp"] != build_pe["coffTimestamp"]:
        raise AuditError("MSVC map timestamp does not match the build/install fea.exe COFF timestamp")

    # Rehash all consumed identities after parsing so the receipt refers to a
    # stable byte range across the entire audit, not only initial snapshots.
    for path, label, expected_sha256, max_bytes in (
            (map_path, "map", actual_map_sha256, MAX_MAP_BYTES),
            (source_path, "source proof", actual_source_sha256, MAX_SOURCE_PROOF_BYTES),
            (report_path, "inventory report", actual_report_sha256, MAX_REPORT_BYTES),
            (Path(build_pe["path"]), "Release build fea.exe", build_pe["sha256"], MAX_PE_BYTES),
            (Path(install_pe["path"]), "OFF install fea.exe", install_pe["sha256"], MAX_PE_BYTES)):
        final_sha256, final_size = _hash_file(path, label, max_bytes)
        expected_size = map_size if label == "map" else source_size if label == "source proof" else \
            report_size if label == "inventory report" else build_pe["sizeBytes"]
        if final_sha256 != expected_sha256 or final_size != expected_size:
            raise AuditError("{} changed during the complete map audit".format(label))

    gate_status = "FAIL" if map_result["forbiddenRowCount"] else "PASS"
    return {
        "schemaVersion": 1,
        "receiptType": "AGENT_OFF_MSVC_LINK_MAP_GATE",
        "linkMapGateStatus": gate_status,
        "variant": "OFF",
        "goPermitted": False,
        "scope": "Pinned MSVC Release fea-link.map symbol/member rows and the exact Release/install fea.exe bytes only",
        "externalPins": {
            "map": {"path": str(map_path), "sizeBytes": map_size, "sha256": actual_map_sha256,
                    "pinSource": "required command-line SHA-256"},
            "sourceProof": {"path": str(source_path), "sizeBytes": source_size,
                            "sha256": actual_source_sha256,
                            "pinSource": "required command-line SHA-256",
                            "scope": "Opaque source identity evidence bytes; completeness and trust origin are external"},
            "inventoryReport": {"path": str(report_path), "sizeBytes": report_size,
                                "sha256": actual_report_sha256,
                                "pinSource": "required command-line SHA-256"},
        },
        "peBinding": {
            "build": build_pe,
            "installed": install_pe,
            "byteIdentical": True,
            "mapTimestampMatchesCoffTimestamp": True,
            "mapHeaderTimestamp": map_result["timestamp"],
        },
        "pinnedInventoryReport": {
            "variant": report["variant"],
            "inventoryStatus": report["inventoryStatus"],
            "releaseQualificationStatus": report["releaseQualificationStatus"],
            "reportWasModified": False,
        },
        "map": map_result,
        "evidenceLimit": (
            "A PASS covers only the externally pinned map text and build/install PE bytes described above. "
            "It does not prove source-to-binary reproducibility, exclude symbols absent from the map, "
            "inspect embedded strings or arbitrary binary data, establish runtime plugin loading, or change "
            "the pinned v43 report's releaseQualificationStatus or any GO decision."
        ),
    }


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", required=True, type=Path,
                        help="MSVC Release fea-link.map under the matching build root")
    parser.add_argument("--map-sha256", required=True,
                        help="External SHA-256 pin for the complete map bytes")
    parser.add_argument("--build-exe", required=True, type=Path,
                        help="Release build's fea.exe identified by the pinned OFF inventory report")
    parser.add_argument("--install-exe", required=True, type=Path,
                        help="OFF install's bin/fea.exe identified by the pinned OFF inventory report")
    parser.add_argument("--source-proof", required=True, type=Path,
                        help="Externally governed source identity file or manifest")
    parser.add_argument("--source-sha256", required=True,
                        help="External SHA-256 pin for --source-proof")
    parser.add_argument("--report", required=True, type=Path,
                        help="Pinned inventory_v43 OFF report used to bind build/install paths and bytes")
    parser.add_argument("--report-sha256", required=True,
                        help="External SHA-256 pin for the complete v43 report bytes")
    parser.add_argument("--output", required=True, type=Path,
                        help="New independent JSON receipt path; existing files are never overwritten")
    return parser


def write_receipt(output_value, receipt, protected_roots=()):
    output = _absolute_path(output_value)
    if output.exists() or output.is_symlink():
        raise AuditError("Refusing to overwrite existing output: {}".format(output))
    prospective_output = output.parent.resolve(strict=False) / output.name
    for root_value in protected_roots:
        protected_root = _absolute_path(root_value).resolve(strict=False)
        if _path_is_within(prospective_output, protected_root):
            raise AuditError("Receipt path must be outside the audited build/install tree: {}".format(
                protected_root))
    output.parent.mkdir(parents=True, exist_ok=True)
    output_parent = output.parent.resolve(strict=True)
    output = output_parent / output.name
    if output.exists() or output.is_symlink():
        raise AuditError("Refusing to overwrite existing output: {}".format(output))
    try:
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
    except OSError as exc:
        raise AuditError("Cannot write receipt {} ({})".format(output, exc))
    return output


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    output = _absolute_path(args.output)
    if output.exists() or output.is_symlink():
        parser.error("refusing to overwrite existing --output: {}".format(output))
    # The documented MSVC layout places the map at <buildRoot>/fea/Release and
    # the installed executable at <installRoot>/bin/fea.exe.  Protect both
    # artifact trees even if the audit later fails while parsing.
    protected_roots = (
        _absolute_path(args.map).parent.parent.parent,
        _absolute_path(args.install_exe).parent.parent,
    )
    try:
        receipt = audit_off_link_map(
            args.map, args.build_exe, args.install_exe,
            args.source_proof, args.source_sha256,
            args.report, args.report_sha256,
            args.map_sha256,
        )
    except AuditError as exc:
        receipt = {
            "schemaVersion": 1,
            "receiptType": "AGENT_OFF_MSVC_LINK_MAP_GATE",
            "linkMapGateStatus": "FAIL",
            "variant": "OFF",
            "goPermitted": False,
            "fatalError": str(exc),
            "evidenceLimit": "Input validation or fail-closed map parsing did not establish the requested scope.",
        }
        try:
            receipt_path = write_receipt(output, receipt, protected_roots=protected_roots)
        except AuditError as output_exc:
            print("ERROR: {}".format(output_exc), file=sys.stderr)
            return 2
        print("FAIL: {}".format(exc), file=sys.stderr)
        print("Receipt: {}".format(receipt_path), file=sys.stderr)
        return 1
    try:
        receipt_path = write_receipt(output, receipt, protected_roots=protected_roots)
    except AuditError as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 2
    print("{}: {}".format(receipt["linkMapGateStatus"], receipt_path))
    print("GO permitted: false; pinned v43 releaseQualificationStatus remains {}".format(
        receipt["pinnedInventoryReport"]["releaseQualificationStatus"]))
    return 0 if receipt["linkMapGateStatus"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
