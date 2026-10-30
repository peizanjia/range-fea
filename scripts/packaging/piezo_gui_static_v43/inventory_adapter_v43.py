"""Fail-closed reader for the current v43 schema-v2 Windows install inventory.

The report contract includes explicit files and directories, so empty install
directories remain part of the pinned tree. This module does not infer a file
count, accept the v1 ``prefix`` / ``bytes`` shape, or grant GUI/product admission.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import hashlib
import json
import ntpath
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
from typing import Dict, List, Optional, Tuple


MAX_REPORT_BYTES = 256 * 1024 * 1024
MAX_FILES = 50_000
MAX_TREE_ENTRIES = 100_000
MAX_TREE_DEPTH = 128
MAX_INSTALL_TOTAL_BYTES = 20 * 1024**3
MAX_SINGLE_FILE_BYTES = 10 * 1024**3
HASH_CHUNK_BYTES = 1024 * 1024
MAX_RELATIVE_PATH_CHARS = 1_024
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
REPARSE_FLAG = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
REQUIRED_CHECK_IDS = frozenset((
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
))


class InventoryError(ValueError):
    """Inventory report is malformed, mismatched, or not pinned."""


@dataclass(frozen=True)
class InventoryFile:
    path: str
    size_bytes: int
    sha256: str


@dataclass(frozen=True)
class InventorySnapshot:
    install_root: str
    variant: str
    report_sha256: Optional[str]
    files: Tuple[InventoryFile, ...]
    directories: Tuple[str, ...]

    @property
    def file_count(self) -> int:
        """Return the report's dynamic count, derived from its validated rows."""
        return len(self.files)

    def by_path(self) -> Dict[str, Tuple[int, str]]:
        return {row.path: (row.size_bytes, row.sha256) for row in self.files}


@dataclass(frozen=True)
class InstallTreeReview:
    install_root: str
    variant: str
    report_sha256: str
    file_count: int
    directory_count: int
    total_bytes: int

    def as_json(self) -> Dict[str, object]:
        return {
            "status": "INVENTORY_TREE_MATCH",
            "schemaVersion": 2,
            "variant": self.variant,
            "installRoot": self.install_root,
            "inventoryReportSha256": self.report_sha256,
            "fileCount": self.file_count,
            "directoryCount": self.directory_count,
            "totalBytes": self.total_bytes,
            "guiAdmissionEvaluated": False,
            "goPermitted": False,
            "productActions": {
                "guiLaunched": False,
                "savePerformed": False,
                "runPerformed": False,
                "confirmPerformed": False,
            },
        }


def _canonical_windows_path(value: object, field: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise InventoryError("{} must be a non-empty Windows path".format(field))
    if not ntpath.isabs(value):
        raise InventoryError("{} must be absolute".format(field))
    return ntpath.normcase(ntpath.normpath(value))


def _validate_relative_path(value: object, field: str = "file path") -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise InventoryError("{} must be a non-empty string".format(field))
    if len(value) > MAX_RELATIVE_PATH_CHARS:
        raise InventoryError("{} exceeds the relative-path bound".format(field))
    if ":" in value or "\\" in value or value.startswith("/") or ntpath.splitdrive(value)[0]:
        raise InventoryError("{} must be a relative POSIX path".format(field))
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise InventoryError("{} is not canonical".format(field))
    if PurePosixPath(value).as_posix() != value:
        raise InventoryError("{} is not canonical".format(field))
    return value


def _validate_directories(value: object, file_paths: List[str]) -> Tuple[str, ...]:
    """Validate the explicit, sorted directory inventory including empty dirs."""
    if not isinstance(value, list) or len(value) > MAX_TREE_ENTRIES:
        raise InventoryError("directories must be a bounded list")

    directories: List[str] = []
    directories_by_case: Dict[str, str] = {}
    for candidate in value:
        relative = _validate_relative_path(candidate, "directory path")
        folded = relative.casefold()
        previous = directories_by_case.get(folded)
        if previous is not None:
            raise InventoryError("directories contain a duplicate or case-colliding path")
        directories_by_case[folded] = relative
        directories.append(relative)

    canonical_order = sorted(directories, key=lambda path: (path.casefold(), path))
    if directories != canonical_order:
        raise InventoryError("directories must use canonical case-insensitive path order")

    file_paths_by_case: Dict[str, str] = {path.casefold(): path for path in file_paths}
    for directory in directories:
        file_path = file_paths_by_case.get(directory.casefold())
        if file_path is not None:
            raise InventoryError(
                "inventory file path is also listed as a directory: {} and {}".format(
                    file_path, directory))

        parts = directory.split("/")
        for index in range(1, len(parts)):
            parent = "/".join(parts[:index])
            observed_parent = directories_by_case.get(parent.casefold())
            if observed_parent is None:
                raise InventoryError("directories omit a required parent directory: {}".format(parent))
            if observed_parent != parent:
                raise InventoryError("directory parent path casing differs: {} and {}".format(
                    parent, observed_parent))

    for file_path in file_paths:
        parts = file_path.split("/")[:-1]
        for index in range(1, len(parts) + 1):
            parent = "/".join(parts[:index])
            observed_parent = directories_by_case.get(parent.casefold())
            if observed_parent is None:
                raise InventoryError("directories omit a file parent directory: {}".format(parent))
            if observed_parent != parent:
                raise InventoryError("file parent directory casing differs: {} and {}".format(
                    parent, observed_parent))

    return tuple(directories)


def adapt_inventory_report(
    report: object,
    expected_install_root: str,
    expected_variant: str = "OFF",
) -> InventorySnapshot:
    """Validate a parsed inventory-v43 schema-v2 report.

    ``fileCount`` is checked against the actual row count, and no historical
    install count or inventory-v1 field is consulted.
    """
    if not isinstance(report, dict):
        raise InventoryError("inventory report must be a JSON object")
    if type(report.get("schemaVersion")) is not int or report["schemaVersion"] != 2:
        raise InventoryError("inventory schemaVersion must be 2")
    if report.get("inventoryStatus") != "PASS":
        raise InventoryError("inventoryStatus must be PASS")
    if report.get("variant") != expected_variant or expected_variant != "OFF":
        raise InventoryError("inventory variant must match the required OFF variant")

    expected_root = _canonical_windows_path(expected_install_root, "expected install root")
    report_root = _canonical_windows_path(report.get("installRoot"), "installRoot")
    if report_root != expected_root:
        raise InventoryError("inventory installRoot does not match the candidate install root")

    checks = report.get("checks")
    if not isinstance(checks, list) or not checks:
        raise InventoryError("inventory checks must be a non-empty list")
    observed_ids = set()
    duplicate_ids = set()
    for check in checks:
        if not isinstance(check, dict):
            raise InventoryError("inventory check must be an object")
        check_id = check.get("id")
        if not isinstance(check_id, str) or not check_id:
            raise InventoryError("inventory check id must be a non-empty string")
        if check_id in observed_ids:
            duplicate_ids.add(check_id)
        observed_ids.add(check_id)
        if check.get("status") != "PASS":
            raise InventoryError("inventory contains a non-PASS check: {}".format(check_id))
    duplicate_ids = sorted(duplicate_ids)
    missing_ids = sorted(REQUIRED_CHECK_IDS - observed_ids)
    unexpected_ids = sorted(observed_ids - REQUIRED_CHECK_IDS)
    if duplicate_ids or missing_ids or unexpected_ids or len(checks) != len(REQUIRED_CHECK_IDS):
        raise InventoryError(
            "inventory checks must contain each required producer ID exactly once; "
            "missing={}, unexpected={}, duplicate={}".format(
                missing_ids, unexpected_ids, duplicate_ids))

    rows = report.get("files")
    if not isinstance(rows, list) or not rows or len(rows) > MAX_FILES:
        raise InventoryError("inventory files must be a non-empty bounded list")
    count = report.get("fileCount")
    if type(count) is not int or count != len(rows):
        raise InventoryError("fileCount does not match the dynamic files row count")

    seen = set()
    normalized_rows: List[InventoryFile] = []
    for row in rows:
        if not isinstance(row, dict):
            raise InventoryError("inventory file row must be an object")
        relative = _validate_relative_path(row.get("path"))
        folded = relative.casefold()
        if folded in seen:
            raise InventoryError("inventory contains a duplicate or case-colliding path")
        seen.add(folded)

        size_bytes = row.get("sizeBytes")
        if type(size_bytes) is not int or size_bytes < 0:
            raise InventoryError("sizeBytes must be a non-negative integer")
        digest = row.get("sha256")
        if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
            raise InventoryError("file sha256 must contain exactly 64 hexadecimal characters")
        normalized_rows.append(InventoryFile(relative, size_bytes, digest.lower()))

    normalized_directories = _validate_directories(
        report.get("directories"), [row.path for row in normalized_rows])

    return InventorySnapshot(
        install_root=report_root,
        variant="OFF",
        report_sha256=None,
        files=tuple(normalized_rows),
        directories=normalized_directories,
    )


def _reject_reparse_chain(path: Path) -> None:
    absolute = Path(os.path.abspath(os.fspath(path)))
    for component in reversed((absolute, *absolute.parents)):
        try:
            details = component.lstat()
        except OSError as exc:
            raise InventoryError("report path is unavailable: {} ({})".format(component, exc))
        attributes = getattr(details, "st_file_attributes", 0)
        if stat.S_ISLNK(details.st_mode) or attributes & REPARSE_FLAG:
            raise InventoryError("report path contains a symlink or reparse point: {}".format(component))


def load_inventory_v43(
    report_path: Path,
    expected_install_root: str,
    expected_report_sha256: str,
    expected_variant: str = "OFF",
) -> InventorySnapshot:
    """Load a schema-v2 report only when its external SHA256 pin is supplied."""
    if not isinstance(expected_report_sha256, str) or not SHA256_RE.fullmatch(expected_report_sha256):
        raise InventoryError("a valid externally recorded inventory report SHA256 is required")

    path = Path(report_path)
    _reject_reparse_chain(path)
    try:
        details = path.lstat()
    except OSError as exc:
        raise InventoryError("inventory report is unavailable: {} ({})".format(path, exc))
    if not stat.S_ISREG(details.st_mode):
        raise InventoryError("inventory report must be a regular file")
    if details.st_size > MAX_REPORT_BYTES:
        raise InventoryError("inventory report exceeds the bounded byte size")

    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise InventoryError("cannot read inventory report: {} ({})".format(path, exc))
    if len(raw) != details.st_size:
        raise InventoryError("inventory report changed while being read")
    digest = hashlib.sha256(raw).hexdigest()
    if digest != expected_report_sha256.lower():
        raise InventoryError("inventory report SHA256 differs from the external pin")

    expected_root = _canonical_windows_path(expected_install_root, "expected install root")
    report_path_key = ntpath.normcase(ntpath.normpath(os.path.abspath(os.fspath(path))))
    try:
        inside_install = ntpath.commonpath([report_path_key, expected_root]) == expected_root
    except ValueError:
        inside_install = False
    if inside_install:
        raise InventoryError("inventory report must be outside the install root")

    try:
        report = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise InventoryError("inventory report is not valid UTF-8 JSON: {}".format(exc))
    snapshot = adapt_inventory_report(report, expected_install_root, expected_variant)
    return replace(snapshot, report_sha256=digest)


def _is_reparse_or_link(details: os.stat_result) -> bool:
    return stat.S_ISLNK(details.st_mode) or bool(getattr(details, "st_file_attributes", 0) & REPARSE_FLAG)


def _same_file_state(left: os.stat_result, right: os.stat_result) -> bool:
    if left.st_size != right.st_size or left.st_mtime_ns != right.st_mtime_ns:
        return False
    # Windows Python may expose st_dev/st_ino as zero on one API surface. When
    # both sides provide an identity value, require an exact match.
    for name in ("st_dev", "st_ino"):
        left_value = getattr(left, name, 0)
        right_value = getattr(right, name, 0)
        if left_value and right_value and left_value != right_value:
            return False
    return True


def _same_enumerated_file_state(left: os.stat_result, right: os.stat_result) -> bool:
    # Windows DirEntry.stat() may omit st_dev/st_ino; the subsequent lstat/fstat
    # pair supplies identity checks before and after reading the file bytes.
    return left.st_size == right.st_size and left.st_mtime_ns == right.st_mtime_ns


def _assert_child_within_root(path: Path, root: Path) -> None:
    path_key = ntpath.normcase(ntpath.normpath(os.path.abspath(os.fspath(path))))
    root_key = ntpath.normcase(ntpath.normpath(os.path.abspath(os.fspath(root))))
    try:
        common = ntpath.commonpath((path_key, root_key))
    except ValueError as exc:
        raise InventoryError("install tree entry is on a different volume: {}".format(path)) from exc
    if common != root_key:
        raise InventoryError("install tree path escapes installRoot: {}".format(path))


def _hash_install_file(path: Path, enumerated_stat: os.stat_result) -> Tuple[int, str]:
    _reject_reparse_chain(path)
    try:
        before = path.lstat()
    except OSError as exc:
        raise InventoryError("cannot stat installed file {}: {}".format(path, exc))
    if _is_reparse_or_link(before) or not stat.S_ISREG(before.st_mode):
        raise InventoryError("install tree contains a symlink, reparse point, or non-regular file: {}".format(path))
    if not _same_enumerated_file_state(before, enumerated_stat):
        raise InventoryError("installed file changed between enumeration and hashing: {}".format(path))
    if before.st_size > MAX_SINGLE_FILE_BYTES:
        raise InventoryError("installed file exceeds the bounded size: {}".format(path))

    digest = hashlib.sha256()
    total = 0
    try:
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if not stat.S_ISREG(opened.st_mode) or not _same_file_state(before, opened):
                raise InventoryError("installed file identity changed while opening: {}".format(path))
            while True:
                block = stream.read(HASH_CHUNK_BYTES)
                if not block:
                    break
                total += len(block)
                if total > MAX_SINGLE_FILE_BYTES:
                    raise InventoryError("installed file exceeded the bounded size while hashing: {}".format(path))
                digest.update(block)
            after_open = os.fstat(stream.fileno())
    except OSError as exc:
        raise InventoryError("cannot hash installed file {}: {}".format(path, exc))

    try:
        after_path = path.lstat()
    except OSError as exc:
        raise InventoryError("installed file disappeared while hashing {}: {}".format(path, exc))
    _reject_reparse_chain(path)
    if (_is_reparse_or_link(after_path) or not stat.S_ISREG(after_path.st_mode) or
            total != before.st_size or not _same_file_state(before, after_open) or
            not _same_file_state(before, after_path)):
        raise InventoryError("installed file changed while hashing: {}".format(path))
    return total, digest.hexdigest()


def _scan_install_tree(install_root: Path) -> Tuple[Dict[str, Tuple[int, str]], set, int]:
    root_input = Path(os.path.abspath(os.fspath(install_root)))
    _reject_reparse_chain(root_input)
    if not root_input.is_dir():
        raise InventoryError("installRoot is not a directory: {}".format(root_input))
    try:
        root = root_input.resolve(strict=True)
    except OSError as exc:
        raise InventoryError("cannot resolve installRoot {}: {}".format(root_input, exc))
    _reject_reparse_chain(root)
    if not root.is_dir():
        raise InventoryError("installRoot is not a directory: {}".format(root))

    pending = [(root, 0)]
    files: Dict[str, Tuple[int, str]] = {}
    directories = set()
    folded_paths: Dict[str, str] = {}
    entries_seen = 0
    total_bytes = 0

    while pending:
        directory, depth = pending.pop()
        if depth > MAX_TREE_DEPTH:
            raise InventoryError("install tree exceeds the directory-depth bound")
        _assert_child_within_root(directory, root)
        _reject_reparse_chain(directory)
        try:
            directory_stat = directory.lstat()
        except OSError as exc:
            raise InventoryError("cannot stat install directory {}: {}".format(directory, exc))
        if _is_reparse_or_link(directory_stat) or not stat.S_ISDIR(directory_stat.st_mode):
            raise InventoryError("install tree contains a reparse point or non-directory: {}".format(directory))

        try:
            with os.scandir(directory) as children:
                for entry in children:
                    entries_seen += 1
                    if entries_seen > MAX_TREE_ENTRIES:
                        raise InventoryError("install tree exceeds the bounded entry count")
                    try:
                        entry_stat = entry.stat(follow_symlinks=False)
                    except OSError as exc:
                        raise InventoryError("cannot stat install tree entry {}: {}".format(entry.path, exc))
                    if _is_reparse_or_link(entry_stat):
                        raise InventoryError("install tree contains a symlink or reparse point: {}".format(entry.path))

                    child = Path(entry.path)
                    _assert_child_within_root(child, root)
                    try:
                        relative = child.relative_to(root).as_posix()
                    except ValueError as exc:
                        raise InventoryError("install tree entry escapes installRoot: {}".format(child)) from exc
                    relative = _validate_relative_path(relative)
                    if len(relative) > MAX_RELATIVE_PATH_CHARS:
                        raise InventoryError("install tree path exceeds the relative-path bound: {}".format(relative))
                    child_depth = depth + 1
                    if child_depth > MAX_TREE_DEPTH:
                        raise InventoryError("install tree exceeds the directory-depth bound")

                    folded = relative.casefold()
                    if folded in folded_paths:
                        raise InventoryError("install tree has a case-insensitive path collision: {} and {}".format(
                            folded_paths[folded], relative))
                    folded_paths[folded] = relative

                    if stat.S_ISDIR(entry_stat.st_mode):
                        directories.add(relative)
                        pending.append((child, child_depth))
                    elif stat.S_ISREG(entry_stat.st_mode):
                        if len(files) >= MAX_FILES:
                            raise InventoryError("install tree exceeds the bounded file count")
                        if entry_stat.st_size > MAX_SINGLE_FILE_BYTES:
                            raise InventoryError("installed file exceeds the bounded size: {}".format(relative))
                        total_bytes += entry_stat.st_size
                        if total_bytes > MAX_INSTALL_TOTAL_BYTES:
                            raise InventoryError("install tree exceeds the bounded total byte count")
                        size_bytes, digest = _hash_install_file(child, entry_stat)
                        if size_bytes != entry_stat.st_size:
                            raise InventoryError("installed file size changed while hashing: {}".format(relative))
                        files[relative] = (size_bytes, digest)
                    else:
                        raise InventoryError("install tree contains a special or non-regular entry: {}".format(child))
        except OSError as exc:
            raise InventoryError("cannot enumerate install directory {}: {}".format(directory, exc))

    return files, directories, total_bytes


def _review_snapshot_install_tree(snapshot: InventorySnapshot) -> InstallTreeReview:
    """Privately compare a snapshot created by the pinned-report loader."""
    if snapshot.variant != "OFF":
        raise InventoryError("only an OFF inventory can be used for install-tree review")
    if not snapshot.report_sha256 or not SHA256_RE.fullmatch(snapshot.report_sha256):
        raise InventoryError("install-tree review requires a report loaded with an external SHA256 pin")

    root = Path(snapshot.install_root)
    root_key = _canonical_windows_path(str(root), "install root")
    if root_key != snapshot.install_root:
        raise InventoryError("inventory snapshot installRoot is not canonical")

    actual, actual_directories, total_bytes = _scan_install_tree(root)
    expected = snapshot.by_path()
    expected_directories = set(snapshot.directories)
    expected_directory_case = {path.casefold(): path for path in expected_directories}
    actual_directory_case = {path.casefold(): path for path in actual_directories}
    directory_casing_changes = [
        (expected_directory_case[key], actual_directory_case[key])
        for key in expected_directory_case.keys() & actual_directory_case.keys()
        if expected_directory_case[key] != actual_directory_case[key]
    ]
    if directory_casing_changes:
        raise InventoryError(
            "install directory casing differs from the pinned report: {}".format(
                directory_casing_changes[:5]))

    missing_directories = sorted(expected_directories - actual_directories, key=str.casefold)
    added_directories = sorted(actual_directories - expected_directories, key=str.casefold)
    if missing_directories or added_directories:
        raise InventoryError(
            "install directory set mismatch; missing={}, added={}".format(
                missing_directories[:10], added_directories[:10]))

    expected_case = {path.casefold(): path for path in expected}
    actual_case = {path.casefold(): path for path in actual}
    casing_changes = [
        (expected_case[key], actual_case[key])
        for key in expected_case.keys() & actual_case.keys()
        if expected_case[key] != actual_case[key]
    ]
    if casing_changes:
        raise InventoryError("install path casing differs from the pinned report: {}".format(casing_changes[:5]))

    missing = sorted(expected.keys() - actual.keys(), key=str.casefold)
    added = sorted(actual.keys() - expected.keys(), key=str.casefold)
    if missing or added:
        raise InventoryError("install file set mismatch; missing={}, added={}".format(missing[:10], added[:10]))

    for relative in sorted(expected, key=str.casefold):
        expected_size, expected_digest = expected[relative]
        actual_size, actual_digest = actual[relative]
        if actual_size != expected_size:
            raise InventoryError("installed file size differs from report: {}".format(relative))
        if actual_digest != expected_digest:
            raise InventoryError("installed file SHA256 differs from report: {}".format(relative))

    return InstallTreeReview(
        install_root=snapshot.install_root,
        variant="OFF",
        report_sha256=snapshot.report_sha256,
        file_count=len(actual),
        directory_count=len(actual_directories),
        total_bytes=total_bytes,
    )


def verify_install_tree(
    inventory_report_path: Path,
    expected_install_root: str,
    expected_report_sha256: str,
    expected_variant: str = "OFF",
) -> InstallTreeReview:
    """Load a pinned schema-v2 report and rehash its complete install tree."""
    snapshot = load_inventory_v43(
        inventory_report_path,
        expected_install_root,
        expected_report_sha256,
        expected_variant,
    )
    return _review_snapshot_install_tree(snapshot)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Rehash an OFF install tree against an externally pinned schema-v2 inventory report."
    )
    parser.add_argument("--install-root", required=True, help="Exact installRoot recorded by inventory_v43.py")
    parser.add_argument("--inventory-report", required=True, type=Path, help="Schema-v2 inventory JSON outside the install root")
    parser.add_argument("--inventory-sha256", required=True, help="Externally recorded SHA256 of the exact report bytes")
    parser.add_argument("--variant", required=True, choices=("OFF",), help="Only the Agent-OFF install variant is admitted")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = verify_install_tree(
            args.inventory_report,
            args.install_root,
            args.inventory_sha256,
            args.variant,
        )
    except InventoryError as exc:
        print(json.dumps({
            "status": "INVENTORY_TREE_MISMATCH",
            "error": str(exc),
            "guiAdmissionEvaluated": False,
            "goPermitted": False,
            "productActions": {
                "guiLaunched": False,
                "savePerformed": False,
                "runPerformed": False,
                "confirmPerformed": False,
            },
        }, indent=2), file=sys.stderr)
        return 2
    print(json.dumps(result.as_json(), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
