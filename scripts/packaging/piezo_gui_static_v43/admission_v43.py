"""Fail-closed static admission for a fresh v43 Agent-OFF install.

The admission binds an externally pinned schema-v2 inventory report to the
complete installed file and directory tree, then checks the source/build,
installed image, and original-authority pins.  A successful result is static
evidence only: it always leaves GUI, Save, Run, Confirm, and GO disabled.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import unicodedata
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

from inventory_adapter_v43 import InventoryError, load_inventory_v43, verify_install_tree


SCHEMA = "range.pc11-fresh-run-v43-admission-candidate/v1"
AUTHORITY_COUNT = 10
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
COMMIT_RE = re.compile(r"^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$")
REPARSE_FLAG = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
MAX_JSON_BYTES = 256 * 1024 * 1024
HASH_CHUNK_BYTES = 1024 * 1024
SOURCE_SCOPE_SCHEMA = "range.pc11-v43-source-scope/v1"
SOURCE_SCOPE = {
    "includePrefixes": ["src/", "scripts/packaging/", "scripts/benchmark/", "scripts/ci/", "cmake/"],
    "includeBasenamesAnywhere": [
        "CMakeLists.txt", "CMakePresets.json", "CMakeSettings.json", "vcpkg.json", "vcpkg-configuration.json",
    ],
    "includeExtensionsAnywhere": [".cmake", ".cmake.in"],
    "includeExactPaths": [
        ".github/workflows/windows-regression.yml",
        "docs/PIEZO_CONTROL_DELIVERY.md",
        "docs/piezo_control_plan.json",
    ],
    "excludePrefixes": ["build/", ".git/"],
    "enumeration": (
        "git ls-files -co --exclude-standard -z per repo root and recursive submodule; "
        "omit gitlink entries; hash all included tracked+nonignored untracked regular files; "
        "reject build-like extension outside include rules; reparse/case collision/control chars reject"
    ),
    "pathOrder": "UTF8_BYTE_LEXICOGRAPHIC",
    "treeDigest": "SHA256(UTF8(JSON(sourceFiles, sort_keys=true, separators=(',',':'), ensure_ascii=false)))",
}
BUILD_LIKE_SUFFIXES = (
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ui", ".qrc", ".rc", ".py", ".ps1", ".cmake", ".in",
)

REQUIRED_ACTIONS = (
    "guiLaunched",
    "savePerformed",
    "runPerformed",
    "confirmPerformed",
)
REQUIRED_STATIC_TEST_IDS = (
    "source_preflight",
    "source_freeze",
    "package_tree",
    "module_boundary_build",
    "module_boundary_source",
    "watchdog_preflight",
    "save_predicate_preflight",
    "path_shadow",
    "root_seal",
    "save_predicate",
    "watchdog_contract",
    "sampler_arguments",
    "supervisor",
    "worker_handshake",
    "native_launch_boundary",
    "parser",
    "finalize_integrity",
    "source_static_tests",
    "root_bootstrap",
)


class AdmissionError(ValueError):
    """A static v43 admission pin is missing, malformed, or mismatched."""


class AdmissionIncomplete(AdmissionError):
    """Pinned candidate evidence is structurally valid but not complete enough to pass."""


def _reject_reparse_chain(path: Path) -> None:
    absolute = Path(os.path.abspath(os.fspath(path)))
    for component in (absolute, *absolute.parents):
        try:
            details = component.lstat()
        except OSError as exc:
            raise AdmissionError("pinned path is unavailable: {} ({})".format(component, exc))
        attributes = getattr(details, "st_file_attributes", 0)
        if stat.S_ISLNK(details.st_mode) or attributes & REPARSE_FLAG:
            raise AdmissionError("pinned path contains a symlink or reparse point: {}".format(component))


def _same_file_state(left: os.stat_result, right: os.stat_result) -> bool:
    if left.st_size != right.st_size or left.st_mtime_ns != right.st_mtime_ns:
        return False
    for name in ("st_dev", "st_ino"):
        left_value = getattr(left, name, 0)
        right_value = getattr(right, name, 0)
        if left_value and right_value and left_value != right_value:
            return False
    return True


def _stable_bytes(path: Path, description: str) -> bytes:
    path = Path(path)
    _reject_reparse_chain(path)
    try:
        before = path.lstat()
    except OSError as exc:
        raise AdmissionError("{} is unavailable: {} ({})".format(description, path, exc))
    attributes = getattr(before, "st_file_attributes", 0)
    if not stat.S_ISREG(before.st_mode) or stat.S_ISLNK(before.st_mode) or attributes & REPARSE_FLAG:
        raise AdmissionError("{} must be a regular non-reparse file: {}".format(description, path))
    if before.st_size > MAX_JSON_BYTES:
        raise AdmissionError("{} exceeds the bounded byte size: {}".format(description, path))
    try:
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if not stat.S_ISREG(opened.st_mode) or not _same_file_state(before, opened):
                raise AdmissionError("{} changed while opening: {}".format(description, path))
            raw = stream.read(MAX_JSON_BYTES + 1)
            if len(raw) > MAX_JSON_BYTES:
                raise AdmissionError("{} exceeds the bounded byte size: {}".format(description, path))
            after_open = os.fstat(stream.fileno())
    except OSError as exc:
        raise AdmissionError("cannot read {} {} ({})".format(description, path, exc))
    try:
        after_path = path.lstat()
    except OSError as exc:
        raise AdmissionError("{} disappeared while reading: {} ({})".format(description, path, exc))
    _reject_reparse_chain(path)
    if (not stat.S_ISREG(after_path.st_mode) or len(raw) != before.st_size or
            not _same_file_state(before, after_open) or
            not _same_file_state(before, after_path)):
        raise AdmissionError("{} changed while reading: {}".format(description, path))
    return raw


def sha256_file(path: Path) -> str:
    """Hash one regular file while checking its path and metadata for changes."""
    path = Path(path)
    _reject_reparse_chain(path)
    try:
        before = path.lstat()
    except OSError as exc:
        raise AdmissionError("pinned file is unavailable: {} ({})".format(path, exc))
    attributes = getattr(before, "st_file_attributes", 0)
    if not stat.S_ISREG(before.st_mode) or stat.S_ISLNK(before.st_mode) or attributes & REPARSE_FLAG:
        raise AdmissionError("pinned path is not a regular file: {}".format(path))

    digest = hashlib.sha256()
    total = 0
    try:
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if not stat.S_ISREG(opened.st_mode) or not _same_file_state(before, opened):
                raise AdmissionError("pinned file changed while opening: {}".format(path))
            while True:
                block = stream.read(HASH_CHUNK_BYTES)
                if not block:
                    break
                total += len(block)
                digest.update(block)
            after_open = os.fstat(stream.fileno())
    except OSError as exc:
        raise AdmissionError("cannot hash pinned file {} ({})".format(path, exc))
    try:
        after_path = path.lstat()
    except OSError as exc:
        raise AdmissionError("pinned file disappeared while hashing {} ({})".format(path, exc))
    _reject_reparse_chain(path)
    if (not stat.S_ISREG(after_path.st_mode) or total != before.st_size or
            not _same_file_state(before, after_open) or
            not _same_file_state(before, after_path)):
        raise AdmissionError("pinned file changed while hashing: {}".format(path))
    return digest.hexdigest()


def _sha256(value: object, field: str) -> str:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise AdmissionError("{} must contain exactly 64 hexadecimal characters".format(field))
    return value.lower()


def _commit(value: object, field: str) -> str:
    if not isinstance(value, str) or not COMMIT_RE.fullmatch(value):
        raise AdmissionError("{} must be a 40- or 64-character Git object ID".format(field))
    return value.lower()


def _absolute_file(value: object, field: str) -> Path:
    if not isinstance(value, (str, os.PathLike)):
        raise AdmissionError("{} must be a non-empty absolute file path".format(field))
    try:
        raw = os.fspath(value)
    except TypeError:
        raise AdmissionError("{} must be a non-empty absolute file path".format(field))
    if not raw or "\x00" in raw:
        raise AdmissionError("{} must be a non-empty absolute file path".format(field))
    path = Path(raw)
    if not path.is_absolute():
        raise AdmissionError("{} must be absolute".format(field))
    return path


def _read_pinned_json(path_value: object, digest_value: object, field: str) -> Tuple[Path, Dict[str, Any]]:
    path = _absolute_file(path_value, field + " path")
    expected = _sha256(digest_value, field + " SHA256")
    raw = _stable_bytes(path, field)
    actual = hashlib.sha256(raw).hexdigest()
    if actual != expected:
        raise AdmissionError("{} SHA256 differs from the external pin".format(field))

    def no_duplicate_keys(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
        result: Dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise AdmissionError("{} contains a duplicate JSON key: {}".format(field, key))
            result[key] = value
        return result

    try:
        parsed = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=no_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AdmissionError("{} is not valid UTF-8 JSON: {}".format(field, exc))
    if not isinstance(parsed, dict):
        raise AdmissionError("{} must be a JSON object".format(field))
    return path, parsed


def _verify_file_pin(path_value: object, digest_value: object, field: str) -> Tuple[Path, str]:
    path = _absolute_file(path_value, field + " path")
    expected = _sha256(digest_value, field + " SHA256")
    if sha256_file(path) != expected:
        raise AdmissionError("{} file SHA256 differs from its pin: {}".format(field, path))
    return path, expected


def _relative_posix(value: object, field: str) -> str:
    if not isinstance(value, str) or not value or "\x00" in value:
        raise AdmissionError("{} must be a non-empty relative POSIX path".format(field))
    if "\\" in value or ":" in value or value.startswith("/"):
        raise AdmissionError("{} must be a relative POSIX path".format(field))
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise AdmissionError("{} is not a canonical relative path".format(field))
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise AdmissionError("{} contains a control character".format(field))
    if PurePosixPath(value).as_posix() != value:
        raise AdmissionError("{} is not a canonical relative path".format(field))
    return value


def _same_path(left: object, right: object) -> bool:
    return os.path.normcase(os.path.normpath(os.path.abspath(os.fspath(left)))) == os.path.normcase(
        os.path.normpath(os.path.abspath(os.fspath(right))))


def _verify_manifest_identity(manifest: Mapping[str, Any]) -> None:
    if manifest.get("schema") != SCHEMA:
        raise AdmissionError("candidate manifest schema mismatch")
    if manifest.get("candidateVersion") != "v43":
        raise AdmissionError("candidateVersion must be v43")
    if manifest.get("goPermitted") is not False:
        raise AdmissionError("static v43 admission requires goPermitted=false")
    actions = manifest.get("productActions")
    if not isinstance(actions, dict) or set(actions) != set(REQUIRED_ACTIONS):
        raise AdmissionError("productActions must declare the complete static no-action boundary")
    if any(actions.get(key) is not False for key in REQUIRED_ACTIONS):
        raise AdmissionError("static v43 admission cannot allow a product action")

    boundary = manifest.get("workflowBoundary")
    required_boundary = (
        "staticReadinessDoesNotGrantProductGo",
        "externalIndependentReviewRequired",
        "separateRootTechnicalDecisionRequired",
        "goSealRequiredBeforeAnyProductAction",
    )
    if not isinstance(boundary, dict) or any(boundary.get(key) is not True for key in required_boundary):
        raise AdmissionError("workflowBoundary must preserve independent review and root GO-seal gates")


def _require_no_go_receipt(receipt: Mapping[str, Any], field: str) -> None:
    if receipt.get("candidateVersion") != "v43":
        raise AdmissionError("{} candidateVersion must be v43".format(field))
    if receipt.get("variant") != "OFF":
        raise AdmissionError("{} variant must be OFF".format(field))
    if receipt.get("staticOnly") is not True:
        raise AdmissionError("{} staticOnly must be true".format(field))
    if receipt.get("goPermitted") is not False:
        raise AdmissionError("{} goPermitted must be false".format(field))
    actions = receipt.get("productActions")
    if not isinstance(actions, dict) or set(actions) != set(REQUIRED_ACTIONS) or any(
            actions.get(key) is not False for key in REQUIRED_ACTIONS):
        raise AdmissionError("{} must declare every product action false".format(field))


def _validate_source_freeze_receipt_metadata(freeze: Mapping[str, Any]) -> None:
    if freeze.get("schema") != "range.pc11-v43-source-freeze/v1":
        raise AdmissionError("source freeze receipt schema mismatch")
    if freeze.get("status") != "PASS":
        raise AdmissionError("source freeze receipt status must be PASS")
    _require_no_go_receipt(freeze, "source freeze receipt")
    if freeze.get("configuration") != "Release":
        raise AdmissionError("source freeze receipt configuration must be Release")


def _validate_release_build_receipt_metadata(build: Mapping[str, Any]) -> None:
    if build.get("schema") != "range.pc11-v43-release-build-receipt/v1":
        raise AdmissionError("Release build receipt schema mismatch")
    if build.get("status") != "PASS":
        raise AdmissionError("Release build receipt status must be PASS")
    _require_no_go_receipt(build, "Release build receipt")
    if build.get("configuration") != "Release":
        raise AdmissionError("Release build receipt configuration must be Release")


def _source_tree_digest(rows: Sequence[Mapping[str, Any]]) -> str:
    canonical_rows = []
    for row in rows:
        canonical_rows.append({
            "path": row["path"],
            "sizeBytes": row["sizeBytes"],
            "sha256": str(row["sha256"]).lower(),
        })
    canonical_rows.sort(key=lambda row: row["path"].encode("utf-8"))
    raw = json.dumps(
        canonical_rows,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _git_bytes(repo_root: Path, arguments: Sequence[str], description: str) -> bytes:
    command = ["git", "-C", str(repo_root), "-c", "core.quotepath=false"] + list(arguments)
    try:
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        raise AdmissionError("cannot run Git for {}: {}".format(description, exc))
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", "replace").strip()
        raise AdmissionError("Git failed for {}: {}".format(description, detail[:500]))
    return completed.stdout


def _git_text(repo_root: Path, arguments: Sequence[str], description: str) -> str:
    raw = _git_bytes(repo_root, arguments, description)
    try:
        return raw.decode("utf-8", "strict").strip()
    except UnicodeDecodeError as exc:
        raise AdmissionError("Git returned non-UTF-8 text for {}: {}".format(description, exc))


def _same_file_system_path(left: Path, right: Path) -> bool:
    return os.path.normcase(os.path.normpath(os.path.abspath(os.fspath(left)))) == os.path.normcase(
        os.path.normpath(os.path.abspath(os.fspath(right))))


def _git_stage_entries(repo_root: Path) -> Dict[str, Tuple[str, str]]:
    raw = _git_bytes(repo_root, ("ls-files", "--stage", "-z"), "staged path enumeration")
    result: Dict[str, Tuple[str, str]] = {}
    folded: Dict[str, str] = {}
    for entry in raw.split(b"\x00"):
        if not entry:
            continue
        try:
            metadata, raw_path = entry.split(b"\t", 1)
            mode, object_id, stage = metadata.decode("ascii", "strict").split(" ")
            path = raw_path.decode("utf-8", "strict")
        except (ValueError, UnicodeDecodeError) as exc:
            raise AdmissionError("Git staged path record is malformed: {}".format(exc))
        relative = _relative_posix(path, "Git staged path")
        if stage != "0":
            raise AdmissionError("source repository has an unresolved Git index path: {}".format(relative))
        key = relative.casefold()
        if key in folded:
            raise AdmissionError("Git staged paths contain a case-insensitive collision: {} and {}".format(
                folded[key], relative))
        folded[key] = relative
        result[relative] = (mode, object_id.lower())
    return result


def _source_scope_includes(relative: str) -> bool:
    folded = relative.casefold()
    if _source_scope_excludes(relative):
        return False
    if any(folded.startswith(prefix.casefold()) for prefix in SOURCE_SCOPE["includePrefixes"]):
        return True
    basename = folded.rsplit("/", 1)[-1]
    if basename in {name.casefold() for name in SOURCE_SCOPE["includeBasenamesAnywhere"]}:
        return True
    if any(folded.endswith(suffix.casefold()) for suffix in SOURCE_SCOPE["includeExtensionsAnywhere"]):
        return True
    return relative in SOURCE_SCOPE["includeExactPaths"]


def _source_scope_excludes(relative: str) -> bool:
    folded = relative.casefold()
    return any(folded.startswith(prefix.casefold()) for prefix in SOURCE_SCOPE["excludePrefixes"])


def _validate_enumerated_path(value: str, field: str) -> str:
    relative = _relative_posix(value, field)
    if any(unicodedata.category(character) == "Cc" for character in relative):
        raise AdmissionError("{} contains a control character".format(field))
    return relative


def _enumerate_source_scope(
    source_root: Path,
) -> Tuple[List[Dict[str, Any]], Dict[str, Dict[str, Any]], str]:
    """Re-enumerate tracked and nonignored untracked source bytes, including nested submodules."""
    actual_rows: Dict[str, Dict[str, Any]] = {}
    folded_paths: Dict[str, str] = {}
    actual_submodules: Dict[str, Dict[str, Any]] = {}

    def visit(repo_root: Path, global_prefix: str) -> Tuple[List[Dict[str, Any]], str]:
        repo_identity = _git_text(
            repo_root, ("rev-parse", "--show-toplevel", "HEAD"), "repository root and HEAD")
        identity_lines = repo_identity.splitlines()
        if len(identity_lines) != 2:
            raise AdmissionError("Git returned an invalid repository root/HEAD identity")
        top = Path(identity_lines[0])
        try:
            observed_root = top.resolve(strict=True)
            requested_root = repo_root.resolve(strict=True)
        except OSError as exc:
            raise AdmissionError("cannot resolve Git repository identity: {}".format(exc))
        if not _same_file_system_path(observed_root, requested_root):
            raise AdmissionError("source path is not the root of its own Git repository: {}".format(repo_root))
        head = _commit(identity_lines[1], "Git HEAD")
        stage_entries = _git_stage_entries(repo_root)
        gitlinks = {
            path: object_id
            for path, (mode, object_id) in stage_entries.items()
            if mode == "160000"
        }

        listed = _git_bytes(
            repo_root,
            ("ls-files", "-co", "--exclude-standard", "-z"),
            "tracked and nonignored untracked path enumeration",
        )
        local_paths: List[str] = []
        local_case: Dict[str, str] = {}
        for raw_path in listed.split(b"\x00"):
            if not raw_path:
                continue
            try:
                decoded = raw_path.decode("utf-8", "strict")
            except UnicodeDecodeError as exc:
                raise AdmissionError("Git source path is not UTF-8: {}".format(exc))
            if decoded.endswith("/") and decoded[:-1] in gitlinks:
                continue
            relative = _validate_enumerated_path(decoded, "Git source path")
            folded = relative.casefold()
            if folded in local_case:
                raise AdmissionError("Git source paths contain a case-insensitive collision: {} and {}".format(
                    local_case[folded], relative))
            local_case[folded] = relative
            local_paths.append(relative)

        local_source_rows: List[Dict[str, Any]] = []
        for local in local_paths:
            global_path = _validate_enumerated_path(
                global_prefix + local if global_prefix else local,
                "repository source path",
            )
            if local in gitlinks:
                continue
            if _source_scope_excludes(global_path):
                continue
            if not _source_scope_includes(global_path):
                folded = global_path.casefold()
                if any(folded.endswith(suffix) for suffix in BUILD_LIKE_SUFFIXES):
                    raise AdmissionError("build-like source file is outside the frozen source scope: {}".format(
                        global_path))
                continue

            key = global_path.casefold()
            if key in folded_paths:
                raise AdmissionError("source files contain a case-insensitive collision: {} and {}".format(
                    folded_paths[key], global_path))
            folded_paths[key] = global_path
            file_path = repo_root.joinpath(*local.split("/"))
            _reject_reparse_chain(file_path)
            try:
                details = file_path.lstat()
            except OSError as exc:
                raise AdmissionError("enumerated source file is unavailable: {} ({})".format(global_path, exc))
            attributes = getattr(details, "st_file_attributes", 0)
            if not stat.S_ISREG(details.st_mode) or stat.S_ISLNK(details.st_mode) or attributes & REPARSE_FLAG:
                raise AdmissionError("source scope contains a non-regular or reparse file: {}".format(global_path))
            digest = sha256_file(file_path)
            row = {
                "path": global_path,
                "sizeBytes": details.st_size,
                "sha256": digest,
            }
            actual_rows[global_path] = row
            local_source_rows.append({
                "path": local,
                "sizeBytes": details.st_size,
                "sha256": digest,
            })

        for local_path, registered in sorted(gitlinks.items(), key=lambda item: item[0].encode("utf-8")):
            relative = _validate_enumerated_path(
                global_prefix + local_path if global_prefix else local_path,
                "Git submodule path",
            )
            child = repo_root.joinpath(*local_path.split("/"))
            _reject_reparse_chain(child)
            if not child.is_dir():
                raise AdmissionError("recursive submodule is not initialized: {}".format(relative))
            child_root = child.resolve(strict=True)
            _, child_head = visit(child_root, relative + "/")
            actual_submodules[relative] = {
                "path": relative,
                "registeredCommit": _commit(registered, "{} registered commit".format(relative)),
                "headCommit": child_head,
            }
        return local_source_rows, head

    _, root_head = visit(source_root, "")
    rows = [actual_rows[path] for path in sorted(actual_rows, key=lambda item: item.encode("utf-8"))]
    for module_path, module in actual_submodules.items():
        prefix = module_path + "/"
        module_rows = [
            {"path": row["path"][len(prefix):], "sizeBytes": row["sizeBytes"], "sha256": row["sha256"]}
            for row in rows
            if row["path"].startswith(prefix)
        ]
        module["sourceTreeSha256"] = _source_tree_digest(module_rows)
    return rows, actual_submodules, root_head


def _validate_submodules(value: object, field: str) -> List[Dict[str, Any]]:
    if not isinstance(value, list):
        raise AdmissionError("{} must be a list".format(field))
    result: List[Dict[str, Any]] = []
    seen_paths = set()
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            raise AdmissionError("{}[{}] must be an object".format(field, index))
        required_keys = {"path", "registeredCommit", "headCommit", "worktreeStatus", "sourceTreeSha256"}
        if set(item) != required_keys:
            raise AdmissionError("{}[{}] has an invalid submodule row shape".format(field, index))
        path = _relative_posix(item.get("path"), "{}[{}].path".format(field, index))
        folded = path.casefold()
        if folded in seen_paths:
            raise AdmissionError("{} contains duplicate or case-colliding submodule paths".format(field))
        seen_paths.add(folded)
        registered = _commit(item.get("registeredCommit"), "{} registeredCommit".format(path))
        head = _commit(item.get("headCommit"), "{} headCommit".format(path))
        source_tree = _sha256(item.get("sourceTreeSha256"), "{} sourceTreeSha256".format(path))
        worktree_status = item.get("worktreeStatus")
        if not isinstance(worktree_status, str) or not worktree_status:
            raise AdmissionError("{} worktreeStatus must be a non-empty string".format(path))
        result.append({
            "path": path,
            "registeredCommit": registered,
            "headCommit": head,
            "worktreeStatus": worktree_status,
            "sourceTreeSha256": source_tree,
        })
    result.sort(key=lambda row: row["path"].encode("utf-8"))
    return result


def _verify_source_and_build(pins: Mapping[str, Any]) -> Dict[str, Any]:
    if not isinstance(pins, dict):
        raise AdmissionError("sourceAndAuthorityPins must be an object")

    source_root = _absolute_file(pins.get("sourceRootPath"), "sourceRootPath")
    if not source_root.is_dir():
        raise AdmissionError("sourceRootPath must be an existing directory")
    _reject_reparse_chain(source_root)

    _, freeze = _read_pinned_json(
        pins.get("sourceFreezeReceiptPath"),
        pins.get("sourceFreezeReceiptSha256"),
        "source freeze receipt",
    )
    _validate_source_freeze_receipt_metadata(freeze)
    freeze_source_root = _absolute_file(freeze.get("sourceRootPath"), "source freeze sourceRootPath")
    if not _same_path(source_root, freeze_source_root):
        raise AdmissionError("manifest sourceRootPath differs from the source freeze receipt")
    if freeze.get("sourceScopeSchema") != SOURCE_SCOPE_SCHEMA or freeze.get("sourceScope") != SOURCE_SCOPE:
        raise AdmissionError("source freeze receipt does not declare the exact v43 source scope")

    root_commit = _commit(pins.get("rootCommit"), "rootCommit")
    freeze_root_commit = _commit(freeze.get("rootCommit"), "source freeze rootCommit")
    if root_commit != freeze_root_commit:
        raise AdmissionError("manifest rootCommit differs from the source freeze receipt")
    submodules = _validate_submodules(pins.get("submoduleCommits"), "submoduleCommits")
    freeze_submodules = _validate_submodules(freeze.get("submoduleCommits"), "source freeze submoduleCommits")
    if submodules != freeze_submodules:
        raise AdmissionError("manifest submoduleCommits differ from the source freeze receipt")

    build_path, build = _read_pinned_json(
        pins.get("releaseBuildReceiptPath"),
        pins.get("releaseBuildReceiptSha256"),
        "Release build receipt",
    )
    _validate_release_build_receipt_metadata(build)

    source_rows = freeze.get("sourceFiles")
    if not isinstance(source_rows, list) or not source_rows:
        raise AdmissionError("source freeze sourceFiles must be a non-empty list")
    normalized_source_rows: List[Dict[str, Any]] = []
    seen_source_paths = set()
    for index, row in enumerate(source_rows):
        if not isinstance(row, dict):
            raise AdmissionError("sourceFiles[{}] must be an object".format(index))
        if set(row) != {"path", "sizeBytes", "sha256"}:
            raise AdmissionError("sourceFiles[{}] has an invalid source row shape".format(index))
        relative = _relative_posix(row.get("path"), "sourceFiles[{}].path".format(index))
        folded = relative.casefold()
        if folded in seen_source_paths:
            raise AdmissionError("sourceFiles contains duplicate or case-colliding paths")
        seen_source_paths.add(folded)
        size = row.get("sizeBytes")
        if type(size) is not int or size < 0:
            raise AdmissionError("sourceFiles[{}].sizeBytes must be a non-negative integer".format(index))
        digest = _sha256(row.get("sha256"), "sourceFiles[{}].sha256".format(index))
        if not _source_scope_includes(relative):
            raise AdmissionError("sourceFiles includes a path outside the fixed v43 source scope: {}".format(relative))
        normalized_source_rows.append({"path": relative, "sizeBytes": size, "sha256": digest})

    canonical_source_paths = sorted((row["path"] for row in normalized_source_rows), key=lambda x: x.encode("utf-8"))
    if [row["path"] for row in normalized_source_rows] != canonical_source_paths:
        raise AdmissionError("sourceFiles are not in canonical UTF-8 byte path order")

    actual_source_rows, actual_submodule_map, actual_root_commit = _enumerate_source_scope(source_root)
    if actual_root_commit != root_commit:
        raise AdmissionError("source checkout HEAD differs from the frozen rootCommit")
    expected_source_by_path = {row["path"]: row for row in normalized_source_rows}
    actual_source_by_path = {row["path"]: row for row in actual_source_rows}
    missing_source_paths = sorted(expected_source_by_path.keys() - actual_source_by_path.keys(), key=lambda x: x.encode("utf-8"))
    added_source_paths = sorted(actual_source_by_path.keys() - expected_source_by_path.keys(), key=lambda x: x.encode("utf-8"))
    if missing_source_paths or added_source_paths:
        raise AdmissionError("source scope file set mismatch; missing={}, added={}".format(
            missing_source_paths[:10], added_source_paths[:10]))
    changed_source_paths = [
        path for path in sorted(expected_source_by_path, key=lambda x: x.encode("utf-8"))
        if expected_source_by_path[path] != actual_source_by_path[path]
    ]
    if changed_source_paths:
        raise AdmissionError("source scope file rows differ from freeze receipt: {}".format(changed_source_paths[:10]))

    expected_modules = {row["path"]: row for row in submodules}
    actual_module_paths = set(actual_submodule_map)
    expected_module_paths = set(expected_modules)
    if actual_module_paths != expected_module_paths:
        raise AdmissionError("recursive submodule path set differs from the frozen submoduleCommits")
    for path, actual in actual_submodule_map.items():
        expected = expected_modules[path]
        if (actual["registeredCommit"] != expected["registeredCommit"] or
                actual["headCommit"] != expected["headCommit"] or
                actual["sourceTreeSha256"] != expected["sourceTreeSha256"]):
            raise AdmissionError("recursive submodule identity or source bytes differ from freeze: {}".format(path))

    source_tree = _source_tree_digest(actual_source_rows)
    expected_source_tree = _sha256(pins.get("sourceTreeSha256"), "sourceTreeSha256")
    freeze_source_tree = _sha256(freeze.get("sourceTreeSha256"), "source freeze sourceTreeSha256")
    if source_tree != expected_source_tree or source_tree != freeze_source_tree:
        raise AdmissionError("sourceTreeSha256 does not match the pinned source file rows")

    for path_key, sha_key, label in (
        ("requiredCTestManifestPath", "requiredCTestManifestSha256", "required CTest manifest"),
        ("cmakeCachePath", "cmakeCacheSha256", "CMakeCache"),
    ):
        manifest_path, manifest_sha = _verify_file_pin(pins.get(path_key), pins.get(sha_key), label)
        freeze_path_value = freeze.get(path_key)
        freeze_sha_value = _sha256(freeze.get(sha_key), "source freeze " + sha_key)
        if not _same_path(manifest_path, _absolute_file(freeze_path_value, "source freeze " + path_key)):
            raise AdmissionError("manifest {} differs from the source freeze receipt".format(path_key))
        if manifest_sha != freeze_sha_value:
            raise AdmissionError("manifest {} differs from the source freeze receipt".format(sha_key))

    build_contract = {
        "rootCommit": root_commit,
        "submoduleCommits": submodules,
        "sourceTreeSha256": source_tree,
        "requiredCTestManifestSha256": _sha256(pins.get("requiredCTestManifestSha256"), "requiredCTestManifestSha256"),
        "cmakeCacheSha256": _sha256(pins.get("cmakeCacheSha256"), "cmakeCacheSha256"),
    }
    for key, expected in build_contract.items():
        if key == "submoduleCommits":
            observed = _validate_submodules(build.get(key), "Release build submoduleCommits")
        elif key in ("sourceTreeSha256", "requiredCTestManifestSha256", "cmakeCacheSha256"):
            observed = _sha256(build.get(key), "Release build " + key)
        else:
            observed = _commit(build.get(key), "Release build " + key)
        if observed != expected:
            raise AdmissionError("Release build receipt {} does not match the source/build pins".format(key))

    freeze_build_path = _absolute_file(freeze.get("releaseBuildReceiptPath"), "source freeze releaseBuildReceiptPath")
    freeze_build_sha = _sha256(freeze.get("releaseBuildReceiptSha256"), "source freeze releaseBuildReceiptSha256")
    if not _same_path(freeze_build_path, build_path) or freeze_build_sha != _sha256(
            pins.get("releaseBuildReceiptSha256"), "releaseBuildReceiptSha256"):
        raise AdmissionError("Release build receipt path/SHA differs from the source freeze receipt")

    return {
        "rootCommit": root_commit,
        "submoduleCount": len(submodules),
        "sourceFileCount": len(normalized_source_rows),
        "sourceTreeSha256": source_tree,
        "sourceFreezeReceiptSha256": _sha256(pins.get("sourceFreezeReceiptSha256"), "sourceFreezeReceiptSha256"),
        "releaseBuildReceiptSha256": _sha256(pins.get("releaseBuildReceiptSha256"), "releaseBuildReceiptSha256"),
    }


def _authority_rows(report: Mapping[str, Any]) -> List[Dict[str, str]]:
    if isinstance(report.get("originalAuthorities"), list):
        rows = list(report["originalAuthorities"])
    elif isinstance(report.get("originalAuthorityBefore"), list):
        rows = list(report["originalAuthorityBefore"])
        after = report.get("originalAuthorityAfter")
        if after is not None:
            if not isinstance(after, list):
                raise AdmissionError("authority report originalAuthorityAfter must be a list")
            def identity(items: Sequence[object], field: str) -> Dict[str, str]:
                output: Dict[str, str] = {}
                for index, item in enumerate(items):
                    if not isinstance(item, dict):
                        raise AdmissionError("{}[{}] must be an object".format(field, index))
                    path = _absolute_file(item.get("path"), "{}[{}].path".format(field, index))
                    key = os.path.normcase(os.path.normpath(os.path.abspath(os.fspath(path))))
                    digest = _sha256(item.get("sha256"), "{}[{}].sha256".format(field, index))
                    if key in output:
                        raise AdmissionError("{} contains duplicate authority paths".format(field))
                    output[key] = digest
                return output
            if identity(rows, "originalAuthorityBefore") != identity(after, "originalAuthorityAfter"):
                raise AdmissionError("original authority before/after reports differ")
    elif isinstance(report.get("originalAuthorityFiles"), list):
        rows = list(report["originalAuthorityFiles"])
        auxiliary = report.get("auxiliaryReadOnlyPins", [])
        if not isinstance(auxiliary, list):
            raise AdmissionError("authority report auxiliaryReadOnlyPins must be a list")
        rows.extend(auxiliary)
    else:
        raise AdmissionError("authority report contains no supported original-authority rows")

    result: List[Dict[str, str]] = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise AdmissionError("authority report row {} must be an object".format(index))
        path = _absolute_file(row.get("path"), "authority report row {} path".format(index))
        digest = _sha256(row.get("sha256"), "authority report row {} SHA256".format(index))
        result.append({"path": str(path), "sha256": digest})
    return result


def _verify_authorities(pins: Mapping[str, Any]) -> List[Dict[str, str]]:
    authority_path, report = _read_pinned_json(
        pins.get("authorityReceiptPath"),
        pins.get("authorityReceiptSha256"),
        "original authority receipt",
    )
    rows = _authority_rows(report)
    extras = pins.get("additionalOriginalAuthorities")
    if not isinstance(extras, list):
        raise AdmissionError("additionalOriginalAuthorities must be a list")
    for index, row in enumerate(extras):
        if not isinstance(row, dict):
            raise AdmissionError("additionalOriginalAuthorities[{}] must be an object".format(index))
        path = _absolute_file(row.get("path"), "additionalOriginalAuthorities[{}].path".format(index))
        digest = _sha256(row.get("sha256"), "additionalOriginalAuthorities[{}].sha256".format(index))
        rows.append({"path": str(path), "sha256": digest})

    expected: Dict[str, Tuple[Path, str]] = {}
    for row in rows:
        path = _absolute_file(row.get("path"), "original authority path")
        key = os.path.normcase(os.path.normpath(os.path.abspath(os.fspath(path))))
        if key in expected:
            raise AdmissionError("original authority rows contain duplicate or case-colliding paths")
        expected[key] = (path, _sha256(row.get("sha256"), "original authority SHA256"))
    if len(expected) != AUTHORITY_COUNT:
        raise AdmissionError("original authority set must derive exactly 10 distinct report paths")

    rows_out: List[Dict[str, str]] = []
    for path, expected_sha in expected.values():
        if sha256_file(path) != expected_sha:
            raise AdmissionError("original authority bytes differ from receipt: {}".format(path))
        rows_out.append({"path": str(path.resolve(strict=True)), "sha256": expected_sha})
    rows_out.sort(key=lambda row: os.path.normcase(os.path.normpath(row["path"])))
    return rows_out


def _verify_install_binding(
    manifest: Mapping[str, Any], external_inventory_sha256: str
) -> Tuple[Dict[str, Any], Any, Dict[str, Any]]:
    binding = manifest.get("installBinding")
    layout = manifest.get("installedPathLayout")
    if not isinstance(binding, dict) or not isinstance(layout, dict):
        raise AdmissionError("installBinding and installedPathLayout must be objects")
    if binding.get("requiredVariant") != "OFF" or binding.get("inventorySchemaVersion") != 2:
        raise AdmissionError("v43 static admission requires the OFF schema-v2 inventory variant")
    if binding.get("inventoryStatus") != "PASS":
        raise AdmissionError("manifest inventoryStatus must be PASS for static admission")
    install_root = _absolute_file(binding.get("installRoot"), "installRoot")
    report_path = _absolute_file(binding.get("inventoryReportPath"), "inventoryReportPath")
    manifest_inventory_sha = _sha256(binding.get("inventoryReportSha256"), "inventoryReportSha256")
    if manifest_inventory_sha != external_inventory_sha256:
        raise AdmissionError("external inventory SHA256 differs from the manifest pin")

    gui_relative = _relative_posix(layout.get("guiExecutableRelativePath"), "guiExecutableRelativePath")
    python_relative = _relative_posix(layout.get("privatePythonRelativePath"), "privatePythonRelativePath")
    solver_relative = _relative_posix(layout.get("solverExecutableRelativePath"), "solverExecutableRelativePath")
    provider_root = _relative_posix(layout.get("providerRootRelativePath"), "providerRootRelativePath")
    if gui_relative != "bin/fea.exe" or python_relative != "runtime/python/python.exe" or solver_relative != "bin/fea-solver.exe":
        raise AdmissionError("installed GUI/Python/solver paths differ from the v43 OFF install layout")

    expected_variant = binding.get("requiredVariant")
    try:
        snapshot = load_inventory_v43(report_path, str(install_root), external_inventory_sha256, expected_variant)
        tree_review = verify_install_tree(report_path, str(install_root), external_inventory_sha256, expected_variant)
    except InventoryError as exc:
        raise AdmissionError("schema-v2 OFF inventory/tree review failed: {}".format(exc))
    if tree_review.variant != "OFF" or tree_review.report_sha256 != external_inventory_sha256:
        raise AdmissionError("inventory tree review returned a different report identity or variant")
    _, inventory_report = _read_pinned_json(
        report_path, external_inventory_sha256, "pinned OFF inventory report")

    row_by_path = snapshot.by_path()
    critical_paths = {
        "guiExecutableSha256": gui_relative,
        "privatePythonSha256": python_relative,
        "solverExecutableSha256": solver_relative,
        "productionControlSha256": _relative_posix(layout.get("productionControlRelativePath"), "productionControlRelativePath"),
        "nativeBackendSha256": _relative_posix(layout.get("nativeBackendRelativePath"), "nativeBackendRelativePath"),
    }
    for hash_key, relative in critical_paths.items():
        expected_sha = _sha256(layout.get(hash_key), hash_key)
        observed = row_by_path.get(relative)
        if observed is None or observed[1].lower() != expected_sha:
            raise AdmissionError("installed {} pin is absent or differs from the OFF inventory".format(hash_key))

    provider_prefix = provider_root + "/"
    provider_rows = [
        (row.path[len(provider_prefix):], row.size_bytes, row.sha256.lower())
        for row in snapshot.files
        if row.path.startswith(provider_prefix)
    ]
    control_relative = critical_paths["productionControlSha256"]
    backend_relative = critical_paths["nativeBackendSha256"]
    if not control_relative.startswith(provider_prefix) or not backend_relative.startswith(provider_prefix):
        raise AdmissionError("production-control and native-backend pins must be inside providerRoot")
    if control_relative == backend_relative:
        raise AdmissionError("production-control and native-backend pins must identify distinct provider files")
    provider_rows.sort(key=lambda item: item[0].encode("utf-8"))
    provider_digest_rows = [
        {"path": path, "sizeBytes": size, "sha256": digest}
        for path, size, digest in provider_rows
    ]
    provider_raw = json.dumps(
        provider_digest_rows,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    result = {
        "installRoot": str(install_root),
        "inventoryReportSha256": tree_review.report_sha256,
        "inventoryFileCount": tree_review.file_count,
        "inventoryDirectoryCount": tree_review.directory_count,
        "providerRootRelativePath": provider_root,
        "providerFileCount": len(provider_rows),
        "providerRelativePaths": [row[0] for row in provider_rows],
        "providerTreeSha256": hashlib.sha256(provider_raw).hexdigest(),
        "installedGuiSha256": row_by_path[gui_relative][1],
        "installedPythonSha256": row_by_path[python_relative][1],
        "installedSolverSha256": row_by_path[solver_relative][1],
        "releaseQualificationStatus": "INCOMPLETE_LINK_MAP_NOT_SUPPLIED",
    }
    return result, snapshot, inventory_report


def _candidate_manifest_identity_sha256(manifest: Mapping[str, Any]) -> str:
    binding = manifest.get("installBinding")
    source = manifest.get("sourceAndAuthorityPins")
    if not isinstance(binding, dict) or not isinstance(source, dict):
        raise AdmissionError("candidate manifest identity pins must be objects")
    identity = {
        "schema": SCHEMA,
        "candidateVersion": "v43",
        "installRoot": binding.get("installRoot"),
        "inventoryReportSha256": _sha256(binding.get("inventoryReportSha256"), "inventoryReportSha256"),
        "sourceFreezeReceiptSha256": _sha256(
            source.get("sourceFreezeReceiptSha256"), "sourceFreezeReceiptSha256"),
        "rootCommit": _commit(source.get("rootCommit"), "rootCommit"),
        "sourceTreeSha256": _sha256(source.get("sourceTreeSha256"), "sourceTreeSha256"),
        "releaseBuildReceiptSha256": _sha256(
            source.get("releaseBuildReceiptSha256"), "releaseBuildReceiptSha256"),
    }
    if not isinstance(identity["installRoot"], str) or not identity["installRoot"]:
        raise AdmissionError("candidate manifest identity installRoot must be a non-empty string")
    raw = json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _package_tree_digest(files: Sequence[Mapping[str, Any]], directories: Sequence[str]) -> str:
    canonical_files = []
    for row in files:
        canonical_files.append({
            "path": row["path"],
            "sizeBytes": row["sizeBytes"],
            "sha256": str(row["sha256"]).lower(),
        })
    canonical_files.sort(key=lambda row: row["path"].encode("utf-8"))
    payload = {"files": canonical_files, "directories": list(directories)}
    raw = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _verify_off_link_map_receipt(
    evidence: Mapping[str, Any],
    manifest: Mapping[str, Any],
    inventory_report: Mapping[str, Any],
) -> Tuple[Path, str, Dict[str, Any]]:
    if "offLinkMapReceiptPath" not in evidence or "offLinkMapReceiptSha256" not in evidence:
        raise AdmissionError("candidateEvidencePins must externally pin the OFF link-map v44 receipt")
    receipt_path, receipt = _read_pinned_json(
        evidence.get("offLinkMapReceiptPath"),
        evidence.get("offLinkMapReceiptSha256"),
        "OFF link-map v44 receipt",
    )
    if receipt.get("receiptType") != "AGENT_OFF_MSVC_LINK_MAP_GATE":
        raise AdmissionError("OFF link-map receiptType must be AGENT_OFF_MSVC_LINK_MAP_GATE")
    if type(receipt.get("schemaVersion")) is not int or receipt["schemaVersion"] != 1:
        raise AdmissionError("OFF link-map schemaVersion must be 1")
    if receipt.get("linkMapGateStatus") != "PASS":
        raise AdmissionError("OFF link-map linkMapGateStatus must be PASS")
    if receipt.get("variant") != "OFF":
        raise AdmissionError("OFF link-map variant must be OFF")
    if receipt.get("goPermitted") is not False:
        raise AdmissionError("OFF link-map goPermitted must be false")

    install_binding = manifest.get("installBinding")
    layout = manifest.get("installedPathLayout")
    source_pins = manifest.get("sourceAndAuthorityPins")
    if not isinstance(install_binding, dict) or not isinstance(layout, dict) or not isinstance(source_pins, dict):
        raise AdmissionError("OFF link-map candidate bindings are unavailable")
    report_path = _absolute_file(install_binding.get("inventoryReportPath"), "inventoryReportPath")
    report_sha256 = _sha256(install_binding.get("inventoryReportSha256"), "inventoryReportSha256")
    report_pin = receipt.get("externalPins")
    if not isinstance(report_pin, dict):
        raise AdmissionError("OFF link-map externalPins must be an object")
    inventory_pin = report_pin.get("inventoryReport")
    source_pin = report_pin.get("sourceProof")
    map_pin = report_pin.get("map")
    if not isinstance(inventory_pin, dict):
        raise AdmissionError("OFF link-map externalPins.inventoryReport must be an object")
    inventory_pin_path = _absolute_file(inventory_pin.get("path"), "OFF link-map inventoryReport path")
    if not _same_path(inventory_pin_path, report_path):
        raise AdmissionError("OFF link-map inventoryReport path differs from the manifest")
    if _sha256(inventory_pin.get("sha256"), "OFF link-map inventoryReport SHA256") != report_sha256:
        raise AdmissionError("OFF link-map inventoryReport SHA256 differs from the manifest")

    pinned_inventory = receipt.get("pinnedInventoryReport")
    if not isinstance(pinned_inventory, dict):
        raise AdmissionError("OFF link-map pinnedInventoryReport must be an object")
    if inventory_report.get("variant") != "OFF" or pinned_inventory.get("variant") != "OFF":
        raise AdmissionError("OFF link-map pinned inventory variant must match OFF")
    if inventory_report.get("inventoryStatus") != "PASS" or pinned_inventory.get("inventoryStatus") != "PASS":
        raise AdmissionError("OFF link-map pinned inventory inventoryStatus must be PASS")
    release_status = inventory_report.get("releaseQualificationStatus")
    if not isinstance(release_status, str) or not release_status:
        raise AdmissionError("pinned OFF inventory releaseQualificationStatus must be a non-empty string")
    if release_status.strip().casefold() in ("pass", "go"):
        raise AdmissionError("pinned OFF inventory releaseQualificationStatus must remain non-PASS/non-GO")
    if pinned_inventory.get("releaseQualificationStatus") != release_status:
        raise AdmissionError("OFF link-map releaseQualificationStatus differs from the pinned inventory report")
    if pinned_inventory.get("reportWasModified") is not False:
        raise AdmissionError("OFF link-map reportWasModified must be false")

    freeze_path = _absolute_file(source_pins.get("sourceFreezeReceiptPath"), "sourceFreezeReceiptPath")
    freeze_sha256 = _sha256(source_pins.get("sourceFreezeReceiptSha256"), "sourceFreezeReceiptSha256")
    if not isinstance(source_pin, dict):
        raise AdmissionError("OFF link-map externalPins.sourceProof must be an object")
    source_pin_path = _absolute_file(source_pin.get("path"), "OFF link-map sourceProof path")
    if not _same_path(source_pin_path, freeze_path):
        raise AdmissionError("OFF link-map sourceProof path differs from the source freeze receipt")
    if _sha256(source_pin.get("sha256"), "OFF link-map sourceProof SHA256") != freeze_sha256:
        raise AdmissionError("OFF link-map sourceProof SHA256 differs from the source freeze receipt")

    if not isinstance(map_pin, dict):
        raise AdmissionError("OFF link-map externalPins.map must be an object")
    _absolute_file(map_pin.get("path"), "OFF link-map external map path")
    _sha256(map_pin.get("sha256"), "OFF link-map external map SHA256")
    map_size = map_pin.get("sizeBytes")
    if type(map_size) is not int or map_size <= 0:
        raise AdmissionError("OFF link-map external map sizeBytes must be a positive integer")

    pe_binding = receipt.get("peBinding")
    if not isinstance(pe_binding, dict):
        raise AdmissionError("OFF link-map peBinding must be an object")
    if pe_binding.get("byteIdentical") is not True:
        raise AdmissionError("OFF link-map peBinding.byteIdentical must be true")
    if pe_binding.get("mapTimestampMatchesCoffTimestamp") is not True:
        raise AdmissionError("OFF link-map peBinding.mapTimestampMatchesCoffTimestamp must be true")
    build_pe = pe_binding.get("build")
    installed_pe = pe_binding.get("installed")
    if not isinstance(build_pe, dict) or not isinstance(installed_pe, dict):
        raise AdmissionError("OFF link-map peBinding build and installed identities must be objects")
    install_root = _absolute_file(install_binding.get("installRoot"), "installRoot")
    gui_relative = _relative_posix(layout.get("guiExecutableRelativePath"), "guiExecutableRelativePath")
    expected_installed_path = install_root.joinpath(*gui_relative.split("/"))
    expected_gui_sha256 = _sha256(layout.get("guiExecutableSha256"), "guiExecutableSha256")
    installed_pe_path = _absolute_file(installed_pe.get("path"), "OFF link-map installed PE path")
    build_pe_path = _absolute_file(build_pe.get("path"), "OFF link-map build PE path")
    if not _same_path(installed_pe_path, expected_installed_path):
        raise AdmissionError("OFF link-map installed PE path differs from the manifest GUI executable")
    if _same_path(build_pe_path, installed_pe_path):
        raise AdmissionError("OFF link-map build and installed PE paths must be distinct")
    if _sha256(installed_pe.get("sha256"), "OFF link-map installed PE SHA256") != expected_gui_sha256:
        raise AdmissionError("OFF link-map installed PE SHA256 differs from the manifest GUI executable")
    if _sha256(build_pe.get("sha256"), "OFF link-map build PE SHA256") != expected_gui_sha256:
        raise AdmissionError("OFF link-map build PE SHA256 differs from the installed GUI executable")
    build_size = build_pe.get("sizeBytes")
    installed_size = installed_pe.get("sizeBytes")
    if (type(build_size) is not int or build_size <= 0 or type(installed_size) is not int or installed_size <= 0 or
            build_size != installed_size):
        raise AdmissionError("OFF link-map build/install PE sizes differ")
    if build_pe.get("machine") != "AMD64" or installed_pe.get("machine") != "AMD64":
        raise AdmissionError("OFF link-map build/install PE machine must be AMD64")
    build_timestamp = build_pe.get("coffTimestamp")
    if (not isinstance(build_timestamp, str) or not re.fullmatch(r"[0-9a-fA-F]{8}", build_timestamp) or
            installed_pe.get("coffTimestamp") != build_timestamp):
        raise AdmissionError("OFF link-map build/install PE COFF timestamps differ")

    inventory_files = inventory_report.get("files")
    if not isinstance(inventory_files, list):
        raise AdmissionError("pinned OFF inventory files must be a list")
    gui_inventory_rows = [row for row in inventory_files if isinstance(row, dict) and row.get("path") == gui_relative]
    if len(gui_inventory_rows) != 1:
        raise AdmissionError("pinned OFF inventory must contain exactly one GUI executable row")
    gui_inventory_sha256 = _sha256(gui_inventory_rows[0].get("sha256"), "pinned GUI inventory SHA256")
    if (gui_inventory_sha256 != expected_gui_sha256 or
            gui_inventory_rows[0].get("sizeBytes") != installed_size):
        raise AdmissionError("OFF link-map installed PE size/SHA differs from the pinned GUI inventory row")

    map_result = receipt.get("map")
    if not isinstance(map_result, dict):
        raise AdmissionError("OFF link-map map result must be an object")
    if type(map_result.get("forbiddenRowCount")) is not int or map_result["forbiddenRowCount"] != 0:
        raise AdmissionError("OFF link-map forbiddenRowCount must be zero")
    if map_result.get("forbiddenRows") != []:
        raise AdmissionError("OFF link-map forbiddenRows must be empty")
    if map_result.get("timestamp") != build_timestamp or pe_binding.get("mapHeaderTimestamp") != build_timestamp:
        raise AdmissionError("OFF link-map header timestamp differs from the PE COFF timestamp")

    return receipt_path, _sha256(evidence.get("offLinkMapReceiptSha256"), "offLinkMapReceiptSha256"), receipt


def _verify_package_tree_receipt(
    evidence: Mapping[str, Any],
    manifest: Mapping[str, Any],
    install_snapshot: Any,
    source_result: Mapping[str, Any],
    inventory_sha256: str,
) -> Tuple[Path, str, Dict[str, Any]]:
    receipt_path, receipt = _read_pinned_json(
        evidence.get("packageTreeReceiptPath"),
        evidence.get("packageTreeReceiptSha256"),
        "package-tree receipt",
    )
    if receipt.get("schema") != "range.pc11-v43-package-tree-receipt/v1":
        raise AdmissionError("package-tree receipt must use the v43 schema")
    if receipt.get("status") != "PASS":
        if receipt.get("status") in ("PENDING", "STATIC_INCOMPLETE", "INCOMPLETE"):
            raise AdmissionIncomplete("package-tree receipt is incomplete; static admission cannot pass")
        raise AdmissionError("package-tree receipt status is not PASS")
    _require_no_go_receipt(receipt, "package-tree receipt")
    pins = manifest["sourceAndAuthorityPins"]
    expected = {
        "candidateManifestIdentitySha256": _candidate_manifest_identity_sha256(manifest),
        "sourceFreezeReceiptSha256": source_result["sourceFreezeReceiptSha256"],
        "rootCommit": source_result["rootCommit"],
        "submoduleCommits": _validate_submodules(pins.get("submoduleCommits"), "submoduleCommits"),
        "sourceTreeSha256": source_result["sourceTreeSha256"],
        "requiredCTestManifestSha256": _sha256(
            pins.get("requiredCTestManifestSha256"), "requiredCTestManifestSha256"),
        "cmakeCacheSha256": _sha256(pins.get("cmakeCacheSha256"), "cmakeCacheSha256"),
        "releaseBuildReceiptSha256": source_result["releaseBuildReceiptSha256"],
        "inventoryReportSha256": inventory_sha256,
    }
    for key, value in expected.items():
        if receipt.get(key) != value:
            raise AdmissionError("package-tree receipt {} differs from the candidate pins".format(key))

    raw_files = receipt.get("files")
    raw_directories = receipt.get("directories")
    if not isinstance(raw_files, list) or not isinstance(raw_directories, list):
        raise AdmissionError("package-tree receipt must contain complete files and directories arrays")
    files: List[Dict[str, Any]] = []
    seen = set()
    for index, row in enumerate(raw_files):
        if not isinstance(row, dict) or set(row) != {"path", "sizeBytes", "sha256"}:
            raise AdmissionError("package-tree receipt files[{}] has an invalid row shape".format(index))
        path = _relative_posix(row.get("path"), "package-tree files[{}].path".format(index))
        folded = path.casefold()
        if folded in seen:
            raise AdmissionError("package-tree receipt files contain duplicate or case-colliding paths")
        seen.add(folded)
        size = row.get("sizeBytes")
        if type(size) is not int or size < 0:
            raise AdmissionError("package-tree receipt files[{}].sizeBytes is invalid".format(index))
        digest = _sha256(row.get("sha256"), "package-tree files[{}].sha256".format(index))
        files.append({"path": path, "sizeBytes": size, "sha256": digest})
    if [row["path"] for row in files] != sorted(
            (row["path"] for row in files), key=lambda path: path.encode("utf-8")):
        raise AdmissionError("package-tree receipt files are not in canonical UTF-8 byte path order")
    expected_files = [
        {"path": row.path, "sizeBytes": row.size_bytes, "sha256": row.sha256.lower()}
        for row in install_snapshot.files
    ]
    expected_files.sort(key=lambda row: row["path"].encode("utf-8"))
    files.sort(key=lambda row: row["path"].encode("utf-8"))
    directories = [_relative_posix(path, "package-tree directory") for path in raw_directories]
    if directories != list(install_snapshot.directories):
        raise AdmissionError("package-tree receipt directories differ from the schema-v2 inventory")
    if files != expected_files:
        raise AdmissionError("package-tree receipt files differ from the schema-v2 inventory")
    if type(receipt.get("fileCount")) is not int or receipt["fileCount"] != len(files):
        raise AdmissionError("package-tree receipt fileCount differs from its complete files array")
    if type(receipt.get("directoryCount")) is not int or receipt["directoryCount"] != len(directories):
        raise AdmissionError("package-tree receipt directoryCount differs from its complete directories array")
    expected_digest = _package_tree_digest(files, directories)
    if _sha256(receipt.get("treeDigest"), "package-tree receipt treeDigest") != expected_digest:
        raise AdmissionError("package-tree receipt treeDigest differs from its complete tree arrays")
    return receipt_path, _sha256(evidence.get("packageTreeReceiptSha256"), "packageTreeReceiptSha256"), receipt


def _verify_static_test_receipts(
    evidence: Mapping[str, Any],
    manifest: Mapping[str, Any],
    package_receipt_sha256: str,
    inventory_sha256: str,
    source_result: Mapping[str, Any],
) -> List[str]:
    manifest_required = evidence.get("requiredStaticTestIds")
    if not isinstance(manifest_required, list) or manifest_required != list(REQUIRED_STATIC_TEST_IDS):
        raise AdmissionError("candidate manifest requiredStaticTestIds differ from the v43 static contract")
    entries = evidence.get("staticTestReceipts")
    if not isinstance(entries, list) or len(entries) != 1:
        raise AdmissionError("staticTestReceipts must contain exactly one aggregate v43 static receipt")
    entry = entries[0]
    if not isinstance(entry, dict) or entry.get("name") != "v43-static-suite":
        raise AdmissionError("staticTestReceipts[0] must name the aggregate v43-static-suite")
    _, receipt = _read_pinned_json(entry.get("path"), entry.get("sha256"), "aggregate static-test receipt")
    if receipt.get("schema") != "range.pc11-v43-static-test-receipt/v1":
        raise AdmissionError("aggregate static-test receipt schema mismatch")
    if receipt.get("status") != "PASS":
        if receipt.get("status") in ("PENDING", "STATIC_INCOMPLETE", "INCOMPLETE"):
            raise AdmissionIncomplete("aggregate static-test receipt status is not PASS; static admission is incomplete")
        raise AdmissionError("aggregate static-test receipt status is not PASS")
    _require_no_go_receipt(receipt, "aggregate static-test receipt")
    expected = {
        "candidateManifestIdentitySha256": _candidate_manifest_identity_sha256(manifest),
        "sourceTreeSha256": source_result["sourceTreeSha256"],
        "inventoryReportSha256": inventory_sha256,
        "packageTreeReceiptSha256": package_receipt_sha256,
    }
    for key, value in expected.items():
        if receipt.get(key) != value:
            raise AdmissionError("aggregate static-test receipt {} differs from the candidate pins".format(key))
    required = receipt.get("requiredTestIds")
    if not isinstance(required, list) or required != list(REQUIRED_STATIC_TEST_IDS):
        raise AdmissionIncomplete("aggregate static-test receipt requiredTestIds differ from the v43 static contract")
    results = receipt.get("results")
    if not isinstance(results, list):
        raise AdmissionError("aggregate static-test receipt results must be a list")
    observed: Dict[str, str] = {}
    for index, row in enumerate(results):
        if not isinstance(row, dict):
            raise AdmissionError("aggregate static-test receipt results[{}] must be an object".format(index))
        test_id = row.get("testId")
        status = row.get("status")
        if not isinstance(test_id, str) or not test_id:
            raise AdmissionError("aggregate static-test receipt results[{}].testId is invalid".format(index))
        if test_id in observed:
            raise AdmissionError("aggregate static-test receipt contains a duplicate test ID: {}".format(test_id))
        observed[test_id] = status
    if set(observed) != set(REQUIRED_STATIC_TEST_IDS):
        raise AdmissionIncomplete("aggregate static-test receipt does not contain the complete required test set")
    failed = sorted(test_id for test_id, status in observed.items() if status != "PASS")
    if failed:
        raise AdmissionError("aggregate static-test receipt has non-PASS required tests: {}".format(failed))
    return list(REQUIRED_STATIC_TEST_IDS)


def verify_manifest_file(
    manifest_path: Path,
    expected_manifest_sha256: str,
    expected_inventory_sha256: str,
) -> Dict[str, Any]:
    """Run the OFF install admission with external manifest and inventory pins."""
    manifest_path = Path(manifest_path)
    external_manifest_sha = _sha256(expected_manifest_sha256, "external manifest SHA256")
    external_inventory_sha = _sha256(expected_inventory_sha256, "external inventory SHA256")
    _, manifest = _read_pinned_json(manifest_path, external_manifest_sha, "candidate manifest")
    _verify_manifest_identity(manifest)

    install_result, install_snapshot, inventory_report = _verify_install_binding(manifest, external_inventory_sha)
    pins = manifest.get("sourceAndAuthorityPins")
    if not isinstance(pins, dict):
        raise AdmissionError("sourceAndAuthorityPins must be an object")
    source_result = _verify_source_and_build(pins)
    authority_rows = _verify_authorities(pins.get("authorityBinding", {}))
    authority_raw = json.dumps(
        authority_rows,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")

    evidence = manifest.get("candidateEvidencePins")
    if not isinstance(evidence, dict):
        raise AdmissionError("candidateEvidencePins must be an object")
    off_link_map_path, off_link_map_sha, _ = _verify_off_link_map_receipt(
        evidence, manifest, inventory_report)
    package_path, package_sha, _ = _verify_package_tree_receipt(
        evidence, manifest, install_snapshot, source_result, external_inventory_sha)
    static_test_ids = _verify_static_test_receipts(
        evidence, manifest, package_sha, external_inventory_sha, source_result)

    return {
        "status": "STATIC_OFF_INSTALL_ADMISSION_PASS_NO_PRODUCT_GO",
        "schema": SCHEMA,
        "goPermitted": False,
        "staticAdmissionEvaluated": True,
        "guiAdmissionEvaluated": False,
        "productActions": {key: False for key in REQUIRED_ACTIONS},
        "install": install_result,
        "sourceAndBuild": source_result,
        "originalAuthorityCount": len(authority_rows),
        "originalAuthoritySetSha256": hashlib.sha256(authority_raw).hexdigest(),
        "originalAuthorities": authority_rows,
        "candidateManifestIdentitySha256": _candidate_manifest_identity_sha256(manifest),
        "offLinkMapGateStatus": "PASS",
        "offLinkMapReceiptPath": str(off_link_map_path),
        "offLinkMapReceiptSha256": off_link_map_sha,
        "packageTreeReceiptPath": str(package_path),
        "packageTreeReceiptSha256": package_sha,
        "staticTestReceiptCount": 1,
        "staticTestIds": static_test_ids,
        "productBoundary": "STATIC_ADMISSION_DOES_NOT_AUTHORIZE_GUI_SAVE_RUN_OR_CONFIRM",
        "remainingDecision": "EXTERNAL_INDEPENDENT_REVIEW_AND_SEPARATE_ROOT_GO_SEAL_REQUIRED",
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Verify a pinned v43 OFF install and its static source/build authority pins; never grants product GO."
    )
    parser.add_argument("--manifest", required=True, type=Path, help="Completed v43 admission manifest JSON")
    parser.add_argument("--manifest-sha256", required=True, help="Externally recorded SHA256 of the exact manifest bytes")
    parser.add_argument("--inventory-sha256", required=True, help="Externally recorded SHA256 of the OFF schema-v2 inventory report")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = verify_manifest_file(args.manifest, args.manifest_sha256, args.inventory_sha256)
    except AdmissionIncomplete as exc:
        print(json.dumps({
            "status": "STATIC_INCOMPLETE",
            "error": str(exc),
            "goPermitted": False,
            "guiAdmissionEvaluated": False,
            "productActions": {key: False for key in REQUIRED_ACTIONS},
        }, indent=2), file=sys.stderr)
        return 2
    except (AdmissionError, InventoryError, OSError, ValueError) as exc:
        print(json.dumps({
            "status": "STATIC_OFF_INSTALL_ADMISSION_NO_GO",
            "error": str(exc),
            "goPermitted": False,
            "guiAdmissionEvaluated": False,
            "productActions": {key: False for key in REQUIRED_ACTIONS},
        }, indent=2), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
