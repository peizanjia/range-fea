#!/usr/bin/env python3
"""Fail-closed inventory checks for a Windows Release install tree.

This checks file bytes, selected packaging records, executable provenance
(Release peers or explicit supplier mappings), and PE import names reported by
an explicitly selected dumpbin executable. Optional Release map and GUI
receipts are identity-checked, but remain subject to independent review and
do not establish legal clearance or full runtime plugin coverage.
"""

import argparse
import hashlib
import json
import locale
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


MAX_INSTALL_FILES = 50000
MAX_BUILD_ENTRIES = 200000
MAX_INSTALL_TOTAL_BYTES = 20 * 1024**3
MAX_BUILD_TOTAL_BYTES = 1024 * 1024**3
MAX_SINGLE_FILE_BYTES = 10 * 1024**3
MAX_LINK_MAP_BYTES = 512 * 1024**2
MAX_RELATIVE_PATH_CHARS = 1024
MAX_TREE_DEPTH = 128
HASH_CHUNK_BYTES = 1024 * 1024
DUMPBIN_TIMEOUT_SECONDS = 30
AGENT_OFF_DOCUMENTATION_SUFFIXES = frozenset((".md", ".rst", ".txt", ".pdf", ".docx", ".rtf", ".adoc", ".tex"))
AGENT_OFF_BINARY_SUFFIXES = frozenset((
    ".exe", ".dll", ".lib", ".obj", ".o", ".a", ".so", ".dylib", ".pyd", ".ocx", ".sys",
    ".pdb", ".ilk", ".map", ".bin", ".dat", ".manifest", ".py", ".pyc", ".ps1", ".bat", ".cmd",
))
AGENT_OFF_UI_OBJECT_SUFFIXES = frozenset((
    ".ui", ".qml", ".qrc", ".rcc", ".json", ".xml", ".js", ".css", ".svg", ".png", ".jpg",
    ".jpeg", ".ico", ".webp", ".html", ".htm", ".ini", ".cfg", ".conf", ".ts",
))

QT_SUPPLIER_NOTICES = (
    "COPYING.txt",
    "Copyright.txt",
    "LICENSE",
    "LICENSE.FDL",
    "LICENSE.GPL3-EXCEPT",
)

# The v43 Windows install rule copies OPENSSL_ROOT/bin/openssl.exe directly.
# That supplier executable must carry an explicit source path and SHA-256.
REQUIRED_SUPPLIER_EXE_PATHS = frozenset(("bin/openssl.exe",))

# CMake installs these project targets into bin/. Their provenance must always
# come from the unique matching Release build executable; supplier mappings
# cannot override this requirement, even if a Release peer is missing.
PROJECT_RELEASE_PEER_REQUIRED_PATHS = frozenset((
    "bin/fea.exe",
    "bin/fea-solver.exe",
    "bin/range-control-velocity.exe",
    "bin/range-mesh-worker.exe",
    "bin/range-model-command.exe",
    "bin/range-project.exe",
    "bin/range-native-results.exe",
    "bin/range-workflow-native.exe",
    "bin/range-workflow-isolation-launcher.exe",
))
PROJECT_EXECUTABLE_EXPECTED_PATH_BY_BASENAME = {
    install_path.rsplit("/", 1)[-1].casefold(): install_path
    for install_path in PROJECT_RELEASE_PEER_REQUIRED_PATHS
}
PROJECT_EXECUTABLE_BASENAMES = frozenset(PROJECT_EXECUTABLE_EXPECTED_PATH_BY_BASENAME)

# These 71 paths are the Qt 6.11.1 attribution pages selected for deployment
# by the v43 Windows install proposal. Keep the module subdirectories intact.
QT_ATTRIBUTION_PAGES = tuple("""
qtcore/qtcore-attribution-android-gradle-wrapper.html
qtcore/qtcore-attribution-blake2.html
qtcore/qtcore-attribution-doubleconversion.html
qtcore/qtcore-attribution-easing.html
qtcore/qtcore-attribution-extra-cmake-modules.html
qtcore/qtcore-attribution-forkfd.html
qtcore/qtcore-attribution-kwin.html
qtcore/qtcore-attribution-md4.html
qtcore/qtcore-attribution-md5.html
qtcore/qtcore-attribution-pcre2-sljit.html
qtcore/qtcore-attribution-pcre2.html
qtcore/qtcore-attribution-qeventdispatcher-cf.html
qtcore/qtcore-attribution-rfc6234.html
qtcore/qtcore-attribution-sha1.html
qtcore/qtcore-attribution-sha3-endian.html
qtcore/qtcore-attribution-sha3-keccak.html
qtcore/qtcore-attribution-siphash.html
qtcore/qtcore-attribution-tika-mimetypes.html
qtcore/qtcore-attribution-tinycbor.html
qtcore/qtcore-attribution-tlexpected.html
qtcore/qtcore-attribution-unicode-character-database.html
qtcore/qtcore-attribution-unicode-cldr.html
qtcore/qtcore-attribution-zlib.html
qtdoc/qt-attribution-llvm.html
qtdoc/qt-attribution-llvmpipe.html
qtgui/qtgui-attribution-aglfn.html
qtgui/qtgui-attribution-android-native-style.html
qtgui/qtgui-attribution-cocoa-platform-plugin.html
qtgui/qtgui-attribution-d3d12memoryallocator.html
qtgui/qtgui-attribution-dejayvu.html
qtgui/qtgui-attribution-emoji-segmenter.html
qtgui/qtgui-attribution-freetype-bdf.html
qtgui/qtgui-attribution-freetype-pcf.html
qtgui/qtgui-attribution-freetype-zlib.html
qtgui/qtgui-attribution-freetype.html
qtgui/qtgui-attribution-grayraster.html
qtgui/qtgui-attribution-harfbuzz-ng.html
qtgui/qtgui-attribution-icc-srgb-color-profile.html
qtgui/qtgui-attribution-libjpeg.html
qtgui/qtgui-attribution-libpng.html
qtgui/qtgui-attribution-md4c.html
qtgui/qtgui-attribution-opengl-es2-headers.html
qtgui/qtgui-attribution-opengl-headers.html
qtgui/qtgui-attribution-pixman.html
qtgui/qtgui-attribution-rhi-miniengine-d3d12-mipmap.html
qtgui/qtgui-attribution-smooth-scaling-algorithm.html
qtgui/qtgui-attribution-vulkan-xml-spec.html
qtgui/qtgui-attribution-vulkanmemoryallocator.html
qtgui/qtgui-attribution-webgradients.html
qtgui/qtgui-attribution-wintab.html
qtgui/qtgui-attribution-xcb-xinput.html
qtgui/qtgui-attribution-xserverhelper.html
qtmultimedia/qtmultimedia-attribution-dr-libs.html
qtmultimedia/qtmultimedia-attribution-ffmpeg-boost.html
qtmultimedia/qtmultimedia-attribution-ffmpeg-libjpeg.html
qtmultimedia/qtmultimedia-attribution-ffmpeg-zlib.html
qtmultimedia/qtmultimedia-attribution-ffmpeg.html
qtmultimedia/qtmultimedia-attribution-signalsmith-stretch.html
qtmultimedia/qtmultimedia-attribution-tlfs.html
qtnetwork/qtnetwork-attribution-libpsl.html
qtnetwork/qtnetwork-attribution-psl-data.html
qtpdf/qtpdf-3rdparty-abseil.html
qtpdf/qtpdf-3rdparty-fast-float.html
qtpdf/qtpdf-3rdparty-freetype.html
qtpdf/qtpdf-3rdparty-icu.html
qtpdf/qtpdf-3rdparty-libjpeg-turbo.html
qtpdf/qtpdf-3rdparty-libpng.html
qtpdf/qtpdf-3rdparty-pdfium.html
qtpdf/qtpdf-3rdparty-the-chromium-project.html
qtpdf/qtpdf-3rdparty-zlib.html
qtsvg/qtsvg-attribution-xsvg.html
""".split())

if len(QT_ATTRIBUTION_PAGES) != 71 or len(set(QT_ATTRIBUTION_PAGES)) != 71:
    raise RuntimeError("The frozen Qt attribution list must contain 71 unique pages")


class AuditError(Exception):
    """Input or traversal error that prevents a trustworthy inventory."""


def absolute_path(value):
    path = Path(os.path.abspath(os.fspath(value)))
    if len(os.fspath(path)) > 32767:
        raise AuditError("Absolute path exceeds the Windows path safety bound")
    return path


def is_reparse_or_link(file_stat):
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    attributes = getattr(file_stat, "st_file_attributes", 0)
    return stat.S_ISLNK(file_stat.st_mode) or bool(attributes & reparse_flag)


def is_within(candidate, parent):
    try:
        candidate.relative_to(parent)
        return True
    except ValueError:
        return False


def validate_root(root_value, label):
    root = absolute_path(root_value)
    # Reject a link/junction in any path component so the reported tree cannot
    # silently point outside the caller-selected root.
    for component in reversed((root, *root.parents)):
        try:
            current_stat = component.lstat()
        except OSError as exc:
            raise AuditError("{} is unavailable: {} ({})".format(label, component, exc))
        if is_reparse_or_link(current_stat):
            raise AuditError("{} path contains a symlink or reparse point: {}".format(label, component))
    if not root.is_dir():
        raise AuditError("{} is not a directory: {}".format(label, root))
    return root.resolve(strict=True)


def validate_regular_file(file_value, label):
    file_path = absolute_path(file_value)
    for component in reversed((file_path, *file_path.parents)):
        try:
            component_stat = component.lstat()
        except OSError as exc:
            raise AuditError("{} path is unavailable: {} ({})".format(label, component, exc))
        if is_reparse_or_link(component_stat):
            raise AuditError("{} path contains a symlink or reparse point: {}".format(label, component))
    try:
        file_stat = file_path.lstat()
    except OSError as exc:
        raise AuditError("{} is unavailable: {} ({})".format(label, file_path, exc))
    if is_reparse_or_link(file_stat) or not stat.S_ISREG(file_stat.st_mode):
        raise AuditError("{} must be a regular non-reparse file: {}".format(label, file_path))
    return file_path.resolve(strict=True)


def scan_tree(root, label, *, max_files, max_entries, max_total_bytes):
    """Enumerate files and directories without following links or reparse points."""
    pending = [(root, 0)]
    files = []
    directories = []
    folded_paths = {}
    entries_seen = 0
    total_bytes = 0

    while pending:
        directory, depth = pending.pop()
        if depth > MAX_TREE_DEPTH:
            raise AuditError("{} exceeds the maximum directory depth {}".format(label, MAX_TREE_DEPTH))
        children = []
        try:
            with os.scandir(directory) as scan:
                for entry in scan:
                    entries_seen += 1
                    if entries_seen > max_entries:
                        raise AuditError("{} exceeds the bounded entry count {}".format(label, max_entries))
                    children.append(entry)
        except OSError as exc:
            raise AuditError("Cannot enumerate {} directory {}: {}".format(label, directory, exc))
        children.sort(key=lambda item: item.name.casefold())

        for entry in children:
            path = Path(entry.path)
            try:
                file_stat = entry.stat(follow_symlinks=False)
            except OSError as exc:
                raise AuditError("Cannot stat {} path {}: {}".format(label, path, exc))
            if is_reparse_or_link(file_stat):
                raise AuditError("{} contains a symlink or reparse point: {}".format(label, path))

            relative = path.relative_to(root)
            relative_text = relative.as_posix()
            if len(relative_text) > MAX_RELATIVE_PATH_CHARS:
                raise AuditError("{} path exceeds {} characters: {}".format(
                    label, MAX_RELATIVE_PATH_CHARS, relative_text[:160]))
            folded = relative_text.casefold()
            if folded in folded_paths:
                raise AuditError("{} contains a case-insensitive path collision: {} and {}".format(
                    label, folded_paths[folded], relative_text))
            folded_paths[folded] = relative_text
            child_depth = depth + 1
            if stat.S_ISDIR(file_stat.st_mode):
                if child_depth > MAX_TREE_DEPTH:
                    raise AuditError("{} exceeds the maximum directory depth {}".format(label, MAX_TREE_DEPTH))
                directories.append(relative_text)
                pending.append((path, child_depth))
            elif stat.S_ISREG(file_stat.st_mode):
                if len(files) >= max_files:
                    raise AuditError("{} exceeds the bounded file count {}".format(label, max_files))
                if file_stat.st_size > MAX_SINGLE_FILE_BYTES:
                    raise AuditError("{} file exceeds the bounded size {} bytes: {}".format(
                        label, MAX_SINGLE_FILE_BYTES, relative_text))
                total_bytes += file_stat.st_size
                if total_bytes > max_total_bytes:
                    raise AuditError("{} exceeds the bounded total size {} bytes".format(label, max_total_bytes))
                files.append((path, relative, file_stat))
            else:
                raise AuditError("{} contains a non-regular filesystem entry: {}".format(label, path))

    directories.sort(key=lambda value: (value.casefold(), value))
    return files, directories


def hash_file(path, expected_stat=None):
    try:
        before = path.lstat()
        if is_reparse_or_link(before) or not stat.S_ISREG(before.st_mode):
            raise AuditError("File changed into a symlink or non-regular file while scanning: {}".format(path))
        if before.st_size > MAX_SINGLE_FILE_BYTES:
            raise AuditError("File exceeds the bounded size {} bytes: {}".format(MAX_SINGLE_FILE_BYTES, path))
        if expected_stat is not None and (before.st_size != expected_stat.st_size or
                before.st_mtime_ns != expected_stat.st_mtime_ns):
            raise AuditError("File changed during inventory: {}".format(path))
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if (opened.st_size != before.st_size or
                    getattr(opened, "st_ino", 0) != getattr(before, "st_ino", 0) or
                    getattr(opened, "st_dev", 0) != getattr(before, "st_dev", 0)):
                raise AuditError("File identity changed while opening: {}".format(path))
            total = 0
            while True:
                block = stream.read(HASH_CHUNK_BYTES)
                if not block:
                    break
                total += len(block)
                digest.update(block)
            after = os.fstat(stream.fileno())
        if total != before.st_size or after.st_size != before.st_size or after.st_mtime_ns != before.st_mtime_ns:
            raise AuditError("File changed while hashing: {}".format(path))
        return digest.hexdigest(), total
    except OSError as exc:
        raise AuditError("Cannot hash file {}: {}".format(path, exc))


def inspect_bounded_map_bytes(path):
    """Hash and inspect one bounded map file from the same stable file handle."""
    try:
        before = path.lstat()
        if is_reparse_or_link(before) or not stat.S_ISREG(before.st_mode):
            raise AuditError("Release fea.map changed into a symlink or non-regular file")
        if before.st_size > MAX_LINK_MAP_BYTES:
            raise AuditError("Release fea.map exceeds the bounded size {} bytes".format(MAX_LINK_MAP_BYTES))
        digest = hashlib.sha256()
        total = 0
        prefix = bytearray()
        header_complete = False
        markers = (
            b"Timestamp is", b"Preferred load address is", b"Publics by Value", b"Static symbols",
            b"TetGen:tetgen.obj", b"range-model-lib:rml_tetgen.obj",
        )
        marker_found = {marker: False for marker in markers}
        tail = b""
        tail_size = max(map(len, markers)) - 1
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if (opened.st_size != before.st_size or
                    getattr(opened, "st_ino", 0) != getattr(before, "st_ino", 0) or
                    getattr(opened, "st_dev", 0) != getattr(before, "st_dev", 0)):
                raise AuditError("Release fea.map identity changed while opening")
            while True:
                block = stream.read(HASH_CHUNK_BYTES)
                if not block:
                    break
                total += len(block)
                if total > MAX_LINK_MAP_BYTES:
                    raise AuditError("Release fea.map exceeds the bounded size {} bytes".format(MAX_LINK_MAP_BYTES))
                digest.update(block)
                if not header_complete:
                    newline = block.find(b"\n")
                    prefix.extend(block if newline < 0 else block[:newline])
                    if len(prefix) > 4096:
                        raise AuditError("Release fea.map header exceeds 4096 bytes")
                    header_complete = newline >= 0
                window = tail + block
                for marker in markers:
                    if not marker_found[marker] and marker in window:
                        marker_found[marker] = True
                tail = window[-tail_size:] if tail_size else b""
            after_handle = os.fstat(stream.fileno())
        after_path = path.lstat()
        identity_fields = ("st_size", "st_mtime_ns", "st_ino", "st_dev")
        if total != before.st_size or any(
                getattr(before, field, 0) != getattr(after_handle, field, 0) or
                getattr(before, field, 0) != getattr(after_path, field, 0)
                for field in identity_fields):
            raise AuditError("Release fea.map changed while it was being inspected")
        return digest.hexdigest(), total, bytes(prefix).strip().lower(), marker_found
    except OSError as exc:
        raise AuditError("Cannot inspect Release fea.map {}: {}".format(path, exc))


def inspect_fea_link_map(link_map_value, build_root, inventory, executable_provenance):
    """Bind the Release fea.map to the exact installed/build fea.exe bytes."""
    if link_map_value is None:
        return {
            "status": "UNAVAILABLE_NOT_SUPPLIED",
            "detail": "Provide the matching Release fea.map; the v43 package scan does not create linker maps.",
        }

    link_map = validate_regular_file(link_map_value, "Release fea.map")
    if not is_within(link_map, build_root):
        raise AuditError("--link-map must be inside the selected build root")
    if link_map.suffix.casefold() != ".map" or link_map.name.casefold() != "fea.map":
        raise AuditError("--link-map must be the matching fea.map file")

    executable_row = next((row for row in inventory if row["path"].casefold() == "bin/fea.exe"), None)
    provenance = next((row for row in executable_provenance
                       if row["installedPath"].casefold() == "bin/fea.exe"), None)
    if executable_row is None or provenance is None or provenance.get("status") != "PASS" or \
            provenance.get("kind") != "PROJECT_RELEASE_BUILD":
        raise AuditError("Cannot bind --link-map because bin/fea.exe has no passing Release build provenance")

    release_path = provenance.get("releaseBuildPath")
    if not isinstance(release_path, str) or Path(release_path).name.casefold() != "fea.exe":
        raise AuditError("bin/fea.exe Release build path is unavailable for linker-map binding")
    build_executable = validate_regular_file(build_root.joinpath(*Path(release_path).parts),
                                             "Release fea.exe")
    expected_map = build_executable.with_suffix(".map")
    if link_map != expected_map:
        raise AuditError("--link-map is not the fea.map adjacent to the exact Release fea.exe peer")

    build_digest, build_size = hash_file(build_executable)
    if build_digest != executable_row["sha256"] or build_size != executable_row["sizeBytes"] or \
            build_digest != provenance.get("releaseBuildSha256"):
        raise AuditError("Release fea.exe changed or does not match the installed inventory identity")

    map_digest, map_size, map_first_line, marker_found = inspect_bounded_map_bytes(link_map)
    missing_markers = [marker.decode("ascii") for marker, found in marker_found.items() if not found]
    if map_first_line != b"fea" or missing_markers:
        raise AuditError("Release fea.map is malformed or lacks expected object provenance: {}".format(
            missing_markers or ["fea map header"]))
    return {
        "status": "SUPPLIED_UNREVIEWED",
        "path": str(link_map),
        "sha256": map_digest,
        "sizeBytes": map_size,
        "buildExecutablePath": release_path.replace("\\", "/"),
        "buildExecutableSha256": build_digest,
        "installedExecutablePath": executable_row["path"],
        "installedExecutableSha256": executable_row["sha256"],
        "tetGenArchiveObjectPresent": marker_found[b"TetGen:tetgen.obj"],
        "rangeModelTetGenObjectPresent": marker_found[b"range-model-lib:rml_tetgen.obj"],
        "reviewStatus": "NOT_INDEPENDENTLY_REVIEWED",
        "detail": "Map is adjacent to the Release fea.exe peer whose bytes match the installed executable; an independent map/object-closure review remains required.",
    }


def hash_receipt_artifact(record, label, evidence_root, install_root):
    if not isinstance(record, dict) or not isinstance(record.get("path"), str) or \
            not isinstance(record.get("sha256"), str) or \
            re.fullmatch(r"[0-9a-fA-F]{64}", record["sha256"]) is None or record.get("error") is not None:
        raise AuditError("Installed GUI runtime receipt has an invalid {} record".format(label))
    path = validate_regular_file(record["path"], label)
    if not is_within(path, evidence_root) or is_within(path, install_root):
        raise AuditError("Installed GUI runtime {} is outside its evidence directory".format(label))
    try:
        before = path.lstat()
        digest, size = hash_file(path)
        after = path.lstat()
    except OSError as exc:
        raise AuditError("Cannot verify installed GUI runtime {}: {}".format(label, exc))
    identity_fields = ("st_size", "st_mtime_ns", "st_ino", "st_dev")
    if is_reparse_or_link(after) or any(
            getattr(before, field, 0) != getattr(after, field, 0) for field in identity_fields):
        raise AuditError("Installed GUI runtime {} changed while it was being hashed".format(label))
    if digest != record["sha256"].casefold():
        raise AuditError("Installed GUI runtime {} hash does not match its receipt".format(label))
    return {"path": path.relative_to(evidence_root).as_posix(), "sha256": digest, "sizeBytes": size}


def inspect_gui_runtime_receipt(receipt_value, harness_script_value, install_root, inventory):
    """Report only the installed modules observed in a GUI harness receipt."""
    if receipt_value is None:
        return {
            "status": "NOT_MEASURED",
            "detail": "No installed GUI runtime receipt was supplied.",
        }
    if harness_script_value is None:
        raise AuditError("--gui-harness-script is required with --gui-runtime-receipt")
    receipt_path = validate_regular_file(receipt_value, "installed GUI runtime receipt")
    if is_within(receipt_path, install_root):
        raise AuditError("--gui-runtime-receipt must be outside the install root")
    try:
        receipt_before = receipt_path.lstat()
        if is_reparse_or_link(receipt_before) or not stat.S_ISREG(receipt_before.st_mode):
            raise AuditError("Installed GUI runtime receipt changed into a symlink or non-regular file")
        if receipt_before.st_size > 2 * 1024 * 1024:
            raise AuditError("Installed GUI runtime receipt exceeds the 2 MiB bound")
        with receipt_path.open("rb") as stream:
            receipt_opened = os.fstat(stream.fileno())
            if (receipt_opened.st_size != receipt_before.st_size or
                    getattr(receipt_opened, "st_ino", 0) != getattr(receipt_before, "st_ino", 0) or
                    getattr(receipt_opened, "st_dev", 0) != getattr(receipt_before, "st_dev", 0)):
                raise AuditError("Installed GUI runtime receipt identity changed while opening")
            receipt_bytes = stream.read(2 * 1024 * 1024 + 1)
            receipt_after_handle = os.fstat(stream.fileno())
        receipt_after_path = receipt_path.lstat()
        identity_fields = ("st_size", "st_mtime_ns", "st_ino", "st_dev")
        if len(receipt_bytes) != receipt_before.st_size or len(receipt_bytes) > 2 * 1024 * 1024 or any(
                getattr(receipt_before, field, 0) != getattr(receipt_after_handle, field, 0) or
                getattr(receipt_before, field, 0) != getattr(receipt_after_path, field, 0)
                for field in identity_fields):
            raise AuditError("Installed GUI runtime receipt changed while it was being read")
        receipt_digest = hashlib.sha256(receipt_bytes).hexdigest()
    except OSError as exc:
        raise AuditError("Cannot read installed GUI runtime receipt {}: {}".format(receipt_path, exc))
    try:
        receipt = json.loads(receipt_bytes.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise AuditError("Cannot parse installed GUI runtime receipt: {}".format(exc))
    if not isinstance(receipt, dict) or receipt.get("schemaVersion") != 2:
        raise AuditError("Installed GUI runtime receipt has an unsupported schema")

    harness_script = validate_regular_file(harness_script_value, "installed GUI harness script")
    if is_within(harness_script, install_root):
        raise AuditError("--gui-harness-script must be outside the install root")
    try:
        receipt_harness_script = Path(receipt.get("harnessScriptPath", "")).resolve(strict=True)
    except (OSError, TypeError, ValueError) as exc:
        raise AuditError("Installed GUI runtime receipt harness path is invalid: {}".format(exc))
    if receipt_harness_script != harness_script:
        raise AuditError("Installed GUI runtime receipt names a different harness script")
    harness_digest, harness_size = hash_file(harness_script)
    if not isinstance(receipt.get("harnessScriptSha256"), str) or \
            re.fullmatch(r"[0-9a-fA-F]{64}", receipt["harnessScriptSha256"]) is None or \
            harness_digest != receipt["harnessScriptSha256"].casefold():
        raise AuditError("Installed GUI runtime receipt harness hash does not match the supplied script")

    evidence_root = receipt_path.parent.resolve(strict=True)
    output_files = receipt.get("outputFiles")
    if not isinstance(output_files, list):
        raise AuditError("Installed GUI runtime receipt has no output file hashes")
    hashed_outputs = [hash_receipt_artifact(record, "harness output", evidence_root, install_root)
                      for record in output_files]
    output_names = [Path(row["path"]).name.casefold() for row in hashed_outputs]
    if sorted(output_names) != ["stderr.txt", "stdout.txt"]:
        raise AuditError("Installed GUI runtime receipt must bind stdout.txt and stderr.txt")
    application_log = hash_receipt_artifact(
        receipt.get("applicationLog"), "application log", evidence_root, install_root)
    if Path(application_log["path"]).name.casefold() != "application.log":
        raise AuditError("Installed GUI runtime receipt application log path is unexpected")

    expected_executable = validate_regular_file(install_root / "bin" / "fea.exe", "installed fea.exe")
    try:
        receipt_executable = Path(receipt.get("executable", "")).resolve(strict=True)
    except (OSError, TypeError, ValueError) as exc:
        raise AuditError("Installed GUI runtime receipt executable path is invalid: {}".format(exc))
    if receipt_executable != expected_executable:
        raise AuditError("Installed GUI runtime receipt names a different executable")
    executable_digest, executable_size = hash_file(expected_executable)
    inventory_row = next((row for row in inventory if row["path"].casefold() == "bin/fea.exe"), None)
    if inventory_row is None or executable_digest != inventory_row["sha256"] or \
            executable_digest != receipt.get("executableSha256") or executable_size != inventory_row["sizeBytes"]:
        raise AuditError("Installed GUI runtime receipt does not match the inventoried fea.exe bytes")
    scan = receipt.get("qtLoadErrorScan")
    if receipt.get("outcome") != "passed" or receipt.get("respondingAtCapture") is not True or \
            receipt.get("requiredQtModulesFromInstall") is not True or \
            receipt.get("exitedAfterGracefulClose") is not True or receipt.get("exitCode") != 0 or \
            not isinstance(scan, dict) or scan.get("complete") is not True or scan.get("matchesFound") is not False:
        raise AuditError("Installed GUI runtime receipt did not pass startup, close and Qt-load checks")

    modules = receipt.get("runtimeModules")
    if not isinstance(modules, list):
        raise AuditError("Installed GUI runtime receipt has no module observation list")
    inventory_paths = {row["path"].casefold(): row for row in inventory}
    observed = {}
    for module in modules:
        if not isinstance(module, dict) or not isinstance(module.get("name"), str) or \
                not isinstance(module.get("path"), str):
            raise AuditError("Installed GUI runtime receipt has a malformed module observation")
        module_path = validate_regular_file(module["path"], "loaded Qt module")
        if module_path.name.casefold() != module["name"].casefold():
            raise AuditError("Loaded Qt module name does not match its observed path: {}".format(module["path"]))
        if not is_within(module_path, install_root):
            raise AuditError("Loaded Qt module came from outside the install root: {}".format(module_path))
        relative = module_path.relative_to(install_root).as_posix()
        if relative.casefold() not in inventory_paths:
            raise AuditError("Loaded Qt module is absent from the package inventory: {}".format(relative))
        observed[module["name"].casefold()] = {"name": module["name"], "path": relative}

    required = ("qt6core.dll", "qt6gui.dll", "qt6widgets.dll", "qwindows.dll")
    if any(name not in observed for name in required):
        raise AuditError("Installed GUI runtime receipt is missing a required Qt module")
    expected_module_paths = {
        "qt6core.dll": "bin/Qt6Core.dll",
        "qt6gui.dll": "bin/Qt6Gui.dll",
        "qt6widgets.dll": "bin/Qt6Widgets.dll",
        "qwindows.dll": "bin/platforms/qwindows.dll",
    }
    if any(observed[name]["path"].casefold() != expected_path.casefold()
           for name, expected_path in expected_module_paths.items()):
        raise AuditError("Installed GUI runtime Qt modules did not load from their expected package paths")
    return {
        "status": "UNSIGNED_RECEIPT_QWINDOWS_ONLY",
        "evidenceTrust": "UNSIGNED_LOCAL_HARNESS_RECEIPT",
        "executionAcceptance": "REQUIRES_INDEPENDENT_REVIEW_OF_HARNESS_AND_RAW_LOGS",
        "receiptPath": str(receipt_path),
        "receiptSha256": receipt_digest,
        "receiptSizeBytes": len(receipt_bytes),
        "harnessScriptPath": str(harness_script),
        "harnessScriptSha256": harness_digest,
        "harnessScriptSizeBytes": harness_size,
        "rawEvidenceFiles": sorted(hashed_outputs, key=lambda row: row["path"].casefold()),
        "applicationLog": application_log,
        "executableSha256": executable_digest,
        "observedModules": [observed[name] for name in sorted(observed)],
        "verifiedPlatformPlugin": "qwindows.dll",
        "notEvaluatedPlugins": ["Qt PDF/PDFium", "Qt Multimedia/FFmpeg"],
        "detail": "This validates an unsigned local receipt's identity and asserted qwindows observations. It does not authenticate the harness execution; inspect the recorded harness script and raw logs before accepting runtime evidence. Other plugins are not evaluated.",
    }


def revalidate_install_tree(install_root, initial_inventory, initial_directories):
    """Rescan and rehash the complete install tree after all PE dependency reads."""
    final_files, final_directories = scan_tree(
        install_root,
        "install finalization",
        max_files=MAX_INSTALL_FILES,
        max_entries=MAX_INSTALL_FILES * 2,
        max_total_bytes=MAX_INSTALL_TOTAL_BYTES,
    )
    initial_by_path = {row["path"]: row for row in initial_inventory}
    final_by_path = {}
    folded_paths = {}
    for path, relative, file_stat in final_files:
        relative_text = relative.as_posix()
        folded = relative_text.casefold()
        if folded in folded_paths:
            raise AuditError("Case-insensitive path collision during final install rescan: {} and {}".format(
                folded_paths[folded], relative_text))
        folded_paths[folded] = relative_text
        final_by_path[relative_text] = (path, file_stat)

    added_paths = sorted(set(final_by_path) - set(initial_by_path), key=str.casefold)
    removed_paths = sorted(set(initial_by_path) - set(final_by_path), key=str.casefold)
    initial_directory_set = set(initial_directories)
    final_directory_set = set(final_directories)
    initial_directory_case = {path.casefold(): path for path in initial_directory_set}
    final_directory_case = {path.casefold(): path for path in final_directory_set}
    directory_casing_changes = sorted(
        ((initial_directory_case[key], final_directory_case[key])
         for key in initial_directory_case.keys() & final_directory_case.keys()
         if initial_directory_case[key] != final_directory_case[key]),
        key=lambda pair: pair[0].casefold(),
    )
    added_directories = sorted(final_directory_set - initial_directory_set,
                               key=lambda value: (value.casefold(), value))
    removed_directories = sorted(initial_directory_set - final_directory_set,
                                 key=lambda value: (value.casefold(), value))
    mutated_files = []
    hash_errors = []
    files_rehashed = 0
    for relative_text in sorted(final_by_path, key=str.casefold):
        path, file_stat = final_by_path[relative_text]
        try:
            digest, size = hash_file(path, file_stat)
            files_rehashed += 1
        except AuditError as exc:
            hash_errors.append({"path": relative_text, "error": str(exc)})
            continue
        initial = initial_by_path.get(relative_text)
        if initial is not None and (digest != initial["sha256"] or size != initial["sizeBytes"]):
            mutated_files.append({"path": relative_text,
                                  "expectedSha256": initial["sha256"],
                                  "observedSha256": digest,
                                  "expectedSizeBytes": initial["sizeBytes"],
                                  "observedSizeBytes": size})

    final_libomp_paths = sorted((relative for relative in final_by_path
                                 if "libomp" in relative.casefold()), key=str.casefold)
    details = {
        "initialFileCount": len(initial_inventory),
        "finalFileCount": len(final_files),
        "initialDirectoryCount": len(initial_directories),
        "finalDirectoryCount": len(final_directories),
        "filesRehashed": files_rehashed,
        "addedPaths": added_paths,
        "removedPaths": removed_paths,
        "addedDirectories": added_directories,
        "removedDirectories": removed_directories,
        "directoryCasingChanges": [
            {"initial": before, "final": after}
            for before, after in directory_casing_changes
        ],
        "mutatedFiles": mutated_files,
        "hashErrors": hash_errors,
        "finalLibompPaths": final_libomp_paths,
    }
    passed = not (added_paths or removed_paths or added_directories or removed_directories or
                  directory_casing_changes or mutated_files or hash_errors)
    return passed, details, final_libomp_paths


def parse_dumpbin_dependents(output):
    lines = output.splitlines()
    header = "image has the following dependencies:"
    delay_header = "image has the following delay load dependencies:"
    header_indices = [index for index, line in enumerate(lines) if line.strip().casefold() == header]
    if len(header_indices) != 1:
        raise AuditError("dumpbin output must contain exactly one dependency section header")
    delay_header_indices = [index for index, line in enumerate(lines)
                            if line.strip().casefold() == delay_header]
    if len(delay_header_indices) > 1:
        raise AuditError("dumpbin output contains multiple delay-load dependency section headers")
    if delay_header_indices and delay_header_indices[0] <= header_indices[0]:
        raise AuditError("dumpbin delay-load dependency section must follow the dependency section")

    dependencies = set()
    summary_found = False
    delay_section_found = False
    delay_dependency_count = 0
    for line in lines[header_indices[0] + 1:]:
        candidate = line.strip()
        if not candidate:
            continue
        if candidate.casefold() == "summary":
            summary_found = True
            break
        if candidate.casefold() == delay_header:
            if delay_section_found:
                raise AuditError("dumpbin output contains a repeated delay-load dependency section")
            delay_section_found = True
            continue
        if not re.fullmatch(r'[^<>:"/\\|?*\x00-\x1f]+\.dll', candidate, flags=re.IGNORECASE):
            raise AuditError("Unrecognized entry in dumpbin dependency section: {}".format(candidate[:240]))
        dependencies.add(candidate)
        if delay_section_found:
            delay_dependency_count += 1

    if not summary_found:
        raise AuditError("dumpbin dependency section is missing its Summary terminator")
    if delay_header_indices and not delay_section_found:
        raise AuditError("dumpbin delay-load dependency section appears after its Summary terminator")
    if delay_section_found and delay_dependency_count == 0:
        raise AuditError("dumpbin delay-load dependency section contains no DLL names")
    if not dependencies:
        raise AuditError("dumpbin dependency section contains no DLL names")
    return sorted(dependencies, key=str.casefold)


def run_dumpbin(dumpbin, executable):
    try:
        completed = subprocess.run(
            [str(dumpbin), "/DEPENDENTS", str(executable)],
            capture_output=True,
            text=True,
            encoding=locale.getpreferredencoding(False),
            timeout=DUMPBIN_TIMEOUT_SECONDS,
            check=False,
        )
    except UnicodeError as exc:
        raise AuditError("dumpbin output cannot be decoded using the process locale for {}: {}".format(
            executable, exc))
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AuditError("dumpbin failed for {}: {}".format(executable, exc))
    return completed


def dependencies_from_dumpbin_result(completed, executable):
    if completed.returncode != 0:
        raise AuditError("dumpbin exited {} for {}: {}".format(
            completed.returncode, executable, completed.stderr.strip()[:1000]))
    dependencies = parse_dumpbin_dependents(completed.stdout)
    if not dependencies:
        raise AuditError("dumpbin reported no DLL dependents for {}".format(executable))
    return dependencies


def load_pe_dependencies(dumpbin, executable):
    return dependencies_from_dumpbin_result(run_dumpbin(dumpbin, executable), executable)


def normalize_supplier_install_path(value):
    normalized = str(value).replace("\\", "/")
    parts = normalized.split("/")
    if (not normalized or normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized) or
            any(part in ("", ".", "..") for part in parts)):
        raise AuditError("Supplier executable path must be a normalized install-relative path")
    if len(normalized) > MAX_RELATIVE_PATH_CHARS:
        raise AuditError("Supplier executable install path exceeds the relative path safety bound")
    if any(part.endswith((" ", ".")) or
           any(ord(char) < 32 or char in '<>:\"|?*' for char in part)
           for part in parts):
        raise AuditError("Supplier executable path must use canonical Windows path components")
    if not parts[-1].casefold().endswith(".exe"):
        raise AuditError("Supplier executable install path must end in .exe")
    return "/".join(parts)


def windows_basename_key(relative_path):
    """Return a case-insensitive basename key, folding Win32 trailing-dot/space aliases."""
    basename = str(relative_path).replace("\\", "/").rsplit("/", 1)[-1]
    return basename.rstrip(" .").casefold()


def project_executable_basename_key(relative_path):
    """Match project executable names despite repeated .exe suffix aliases."""
    basename = windows_basename_key(relative_path)
    while basename:
        if basename in PROJECT_EXECUTABLE_BASENAMES:
            return basename
        if not basename.endswith(".exe"):
            break
        basename = basename[:-4]
    return None


def prepare_supplier_exe_mappings(mapping_values, install_root):
    mappings = {}
    for installed_relative, source_value, expected_sha in mapping_values or []:
        installed_relative = normalize_supplier_install_path(installed_relative)
        key = installed_relative.casefold()
        if project_executable_basename_key(installed_relative) is not None:
            raise AuditError("--supplier-exe cannot override a reserved project executable basename: {}".format(
                installed_relative))
        if key in mappings:
            raise AuditError("Duplicate --supplier-exe mapping for {}".format(installed_relative))
        if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha):
            raise AuditError("--supplier-exe SHA-256 must contain exactly 64 hexadecimal characters")
        source = validate_regular_file(source_value, "supplier executable source")
        if is_within(source, install_root):
            raise AuditError("--supplier-exe source must be outside the install root")
        if source.suffix.casefold() != ".exe":
            raise AuditError("--supplier-exe source path must name an .exe file")
        source_digest, source_size = hash_file(source)
        mappings[key] = {
            "installedPath": installed_relative,
            "sourcePath": str(source),
            "expectedSha256": expected_sha.lower(),
            "sourceSha256": source_digest,
            "sourceSizeBytes": source_size,
            "sourceHashMatchesExpected": source_digest == expected_sha.lower(),
        }
    return mappings


def matching_build_peers(build_files, build_root, install_root, executable_name):
    root_is_release = build_root.name.casefold() == "release"
    other_configurations = {"debug", "relwithdebinfo", "minsizerel"}
    wanted = executable_name.casefold()
    candidates = []
    for path, relative, _ in build_files:
        if path.suffix.casefold() != ".exe" or path.name.casefold() != wanted:
            continue
        if is_within(path.resolve(strict=True), install_root):
            continue
        folded_parts = {part.casefold() for part in relative.parts}
        has_release_component = "release" in folded_parts
        if (root_is_release or has_release_component) and not (folded_parts & other_configurations):
            candidates.append((path, relative))
    return candidates


def add_check(checks, check_id, passed, details):
    checks.append({"id": check_id, "status": "PASS" if passed else "FAIL", "details": details})


def audit_inventory(build_root_value, install_root_value, variant, redist_vcomp_value,
                    dumpbin_value, supplier_exe_mappings=None, dumpbin_runner=None,
                    link_map_value=None, gui_runtime_receipt_value=None,
                    gui_harness_script_value=None):
    """Return an inventory report; dumpbin_runner is injectable for fixtures."""
    if variant not in ("ON", "OFF"):
        raise AuditError("variant must be ON or OFF")
    build_root = validate_root(build_root_value, "build root")
    install_root = validate_root(install_root_value, "install root")
    if is_within(build_root, install_root):
        raise AuditError("Build root cannot be inside install root")
    redist_vcomp = validate_regular_file(redist_vcomp_value, "Redist VCOMP140.DLL")
    if redist_vcomp.name.casefold() != "vcomp140.dll":
        raise AuditError("--redist-vcomp must name the supplied vcomp140.dll file")
    dumpbin = validate_regular_file(dumpbin_value, "dumpbin")
    if dumpbin.name.casefold() != "dumpbin.exe":
        raise AuditError("--dumpbin must name the explicit dumpbin.exe executable")
    supplier_mappings = prepare_supplier_exe_mappings(supplier_exe_mappings, install_root)
    if dumpbin_runner is None:
        dumpbin_runner = lambda exe: run_dumpbin(dumpbin, exe)

    install_files, install_directories = scan_tree(
        install_root,
        "install",
        max_files=MAX_INSTALL_FILES,
        max_entries=MAX_INSTALL_FILES * 2,
        max_total_bytes=MAX_INSTALL_TOTAL_BYTES,
    )
    build_files, _build_directories = scan_tree(
        build_root, "build", max_files=MAX_BUILD_ENTRIES,
        max_entries=MAX_BUILD_ENTRIES, max_total_bytes=MAX_BUILD_TOTAL_BYTES)

    inventory = []
    by_relative_path = {}
    dll_files = []
    exe_files = []
    libomp_files = []
    project_executable_locations = {name: [] for name in PROJECT_EXECUTABLE_BASENAMES}
    for path, relative, file_stat in sorted(install_files, key=lambda item: item[1].as_posix().casefold()):
        relative_text = relative.as_posix()
        folded = relative_text.casefold()
        if folded in by_relative_path:
            raise AuditError("Case-insensitive path collision in install tree: {} and {}".format(
                by_relative_path[folded], relative_text))
        by_relative_path[folded] = relative_text
        digest, size = hash_file(path, file_stat)
        inventory.append({"path": relative_text, "sizeBytes": size, "sha256": digest})
        if path.suffix.casefold() == ".dll":
            dll_files.append({"name": path.name, "path": relative_text})
        project_executable_name = project_executable_basename_key(relative_text)
        if project_executable_name is not None:
            project_executable_locations[project_executable_name].append(relative_text)
        if path.suffix.casefold() == ".exe" or project_executable_name is not None:
            exe_files.append((path, relative, digest, size))
        if "libomp" in relative_text.casefold():
            libomp_files.append(relative_text)

    checks = []
    add_check(checks, "installFilesHashed", True, {"fileCount": len(inventory), "totalBytes": sum(
        row["sizeBytes"] for row in inventory)})

    project_install_path_failures = []
    for executable_name, expected_path in sorted(PROJECT_EXECUTABLE_EXPECTED_PATH_BY_BASENAME.items()):
        installed_paths = sorted(project_executable_locations[executable_name], key=str.casefold)
        if len(installed_paths) != 1 or installed_paths[0].casefold() != expected_path.casefold():
            project_install_path_failures.append({
                "executable": executable_name,
                "expectedInstallPath": expected_path,
                "installedPaths": installed_paths,
            })
    add_check(checks, "projectExecutablesAtCmakeInstallPaths", not project_install_path_failures,
              {"expectedProjectExecutableCount": len(PROJECT_EXECUTABLE_EXPECTED_PATH_BY_BASENAME),
               "failures": project_install_path_failures})

    executable_reports = []
    executable_provenance = []
    project_peer_failures = []
    supplier_source_failures = []
    used_supplier_mappings = set()
    executable_integrity_failures = []
    dependency_failures = []
    all_dependencies = []
    for exe_path, exe_relative, exe_digest, exe_size in exe_files:
        installed_relative = exe_relative.as_posix()
        provenance = {"installedPath": installed_relative, "installedSha256": exe_digest,
                      "installedSizeBytes": exe_size}
        supplier_key = installed_relative.casefold()
        supplier_mapping = supplier_mappings.get(supplier_key)
        project_executable_name = project_executable_basename_key(installed_relative)
        if project_executable_name is not None:
            provenance["kind"] = "PROJECT_RELEASE_BUILD"
            candidates = matching_build_peers(
                build_files, build_root, install_root, project_executable_name)
            provenance["releaseBuildCandidates"] = [relative.as_posix() for _, relative in candidates]
            if len(candidates) != 1:
                provenance["status"] = "FAIL"
                provenance["reason"] = "expected exactly one matching Release build executable for reserved project basename"
                project_peer_failures.append(installed_relative)
            else:
                peer_path, peer_relative = candidates[0]
                peer_digest, peer_size = hash_file(peer_path)
                matches = peer_size == exe_size and peer_digest == exe_digest
                provenance.update({"releaseBuildPath": peer_relative.as_posix(),
                                   "releaseBuildSha256": peer_digest,
                                   "releaseBuildSizeBytes": peer_size,
                                   "bytesEqualBySizeAndSha256": matches,
                                   "status": "PASS" if matches else "FAIL"})
                if not matches:
                    project_peer_failures.append(installed_relative)
        elif supplier_mapping is not None:
            used_supplier_mappings.add(supplier_key)
            installed_matches_source = (exe_digest == supplier_mapping["sourceSha256"] and
                                        exe_size == supplier_mapping["sourceSizeBytes"])
            supplier_ok = supplier_mapping["sourceHashMatchesExpected"] and installed_matches_source
            provenance.update({
                "kind": "SUPPLIER",
                "sourcePath": supplier_mapping["sourcePath"],
                "expectedSourceSha256": supplier_mapping["expectedSha256"],
                "observedSourceSha256": supplier_mapping["sourceSha256"],
                "sourceSizeBytes": supplier_mapping["sourceSizeBytes"],
                "sourceHashMatchesExpected": supplier_mapping["sourceHashMatchesExpected"],
                "installedBytesMatchSource": installed_matches_source,
                "sourceHashOrigin": "explicit operator-supplied --supplier-exe value",
                "status": "PASS" if supplier_ok else "FAIL",
            })
            if not supplier_ok:
                supplier_source_failures.append(installed_relative)
        elif supplier_key in REQUIRED_SUPPLIER_EXE_PATHS:
            provenance.update({"kind": "SUPPLIER_REQUIRED", "status": "FAIL",
                               "reason": "this installed executable requires an explicit --supplier-exe source and SHA-256 mapping"})
            supplier_source_failures.append(installed_relative)
        else:
            provenance["kind"] = "PROJECT_RELEASE_BUILD"
            candidates = matching_build_peers(build_files, build_root, install_root, exe_path.name)
            provenance["releaseBuildCandidates"] = [relative.as_posix() for _, relative in candidates]
            if len(candidates) != 1:
                provenance["status"] = "FAIL"
                provenance["reason"] = "no supplier mapping and expected exactly one matching Release build executable"
                project_peer_failures.append(installed_relative)
            else:
                peer_path, peer_relative = candidates[0]
                peer_digest, peer_size = hash_file(peer_path)
                matches = peer_size == exe_size and peer_digest == exe_digest
                provenance.update({"releaseBuildPath": peer_relative.as_posix(),
                                   "releaseBuildSha256": peer_digest,
                                   "releaseBuildSizeBytes": peer_size,
                                   "bytesEqualBySizeAndSha256": matches,
                                   "status": "PASS" if matches else "FAIL"})
                if not matches:
                    project_peer_failures.append(installed_relative)

        integrity = {"inventorySha256": exe_digest, "preDumpbinSha256": None,
                     "postDumpbinSha256": None, "dumpbinAttempted": False,
                     "preDumpbinMatchesInventory": False, "postDumpbinMatchesInventory": False}
        raw_dumpbin_result = None
        try:
            pre_digest, _ = hash_file(exe_path)
            integrity["preDumpbinSha256"] = pre_digest
            integrity["preDumpbinMatchesInventory"] = pre_digest == exe_digest
        except AuditError as exc:
            dependency_failures.append({"path": installed_relative,
                                        "error": "Cannot hash executable immediately before dumpbin: {}".format(exc)})
            executable_integrity_failures.append(installed_relative)
        else:
            integrity["dumpbinAttempted"] = True
            try:
                raw_dumpbin_result = dumpbin_runner(exe_path)
            except AuditError as exc:
                dependency_failures.append({"path": installed_relative, "error": str(exc)})
            finally:
                try:
                    post_digest, _ = hash_file(exe_path)
                    integrity["postDumpbinSha256"] = post_digest
                    integrity["postDumpbinMatchesInventory"] = post_digest == exe_digest
                except AuditError as exc:
                    dependency_failures.append({"path": installed_relative,
                                                "error": "Cannot hash executable immediately after dumpbin: {}".format(exc)})
            if (not integrity["preDumpbinMatchesInventory"] or
                    not integrity["postDumpbinMatchesInventory"]):
                executable_integrity_failures.append(installed_relative)

        dependencies = []
        if raw_dumpbin_result is not None:
            try:
                dependencies = dependencies_from_dumpbin_result(raw_dumpbin_result, exe_path)
                executable_reports.append({"path": installed_relative, "dependents": dependencies})
                all_dependencies.extend({"executable": installed_relative, "name": dependency}
                                        for dependency in dependencies)
            except AuditError as exc:
                dependency_failures.append({"path": installed_relative, "error": str(exc)})
        if not dependencies and not any(row["path"] == installed_relative for row in dependency_failures):
            dependency_failures.append({"path": installed_relative, "error": "no DLL dependents reported"})

        provenance["dumpbinIntegrity"] = integrity
        provenance["peDependents"] = dependencies
        provenance["peDependentsRead"] = bool(dependencies)
        executable_provenance.append(provenance)

    try:
        install_tree_stable, install_tree_revalidation, final_libomp_files = revalidate_install_tree(
            install_root, inventory, install_directories)
    except AuditError as exc:
        install_tree_stable = False
        install_tree_revalidation = {"error": str(exc)}
        final_libomp_files = []
    add_check(checks, "installTreeStableAfterDependencyScan", install_tree_stable,
              install_tree_revalidation)

    unused_supplier_mappings = [mapping["installedPath"] for key, mapping in supplier_mappings.items()
                                if key not in used_supplier_mappings]
    add_check(checks, "projectExecutablesMatchReleasePeers", not project_peer_failures,
              {"projectExecutableCount": sum(1 for row in executable_provenance
                                              if row["kind"] == "PROJECT_RELEASE_BUILD"),
               "failures": project_peer_failures,
               "executables": [row for row in executable_provenance
                               if row["kind"] == "PROJECT_RELEASE_BUILD"]})
    add_check(checks, "supplierExecutableSourcesMatch",
              not supplier_source_failures and not unused_supplier_mappings,
              {"mappingCount": len(supplier_mappings), "failures": supplier_source_failures,
               "unusedMappings": unused_supplier_mappings,
               "executables": [row for row in executable_provenance if row["kind"] == "SUPPLIER"]})
    add_check(checks, "installedExecutableProvenanceComplete",
              bool(exe_files) and all(row["status"] == "PASS" for row in executable_provenance),
              {"installedExecutableCount": len(exe_files), "executables": executable_provenance})
    add_check(checks, "installedExecutablesStableAcrossDumpbin",
              bool(exe_files) and not executable_integrity_failures,
              {"failures": executable_integrity_failures,
               "executables": [{"path": row["installedPath"], "dumpbinIntegrity": row["dumpbinIntegrity"]}
                               for row in executable_provenance]})
    add_check(checks, "peDependentsReadable", bool(exe_files) and not dependency_failures,
              {"failures": dependency_failures, "executables": executable_reports})

    libomp_imports = [row for row in all_dependencies if "libomp" in row["name"].casefold()]
    all_libomp_paths = sorted(set(libomp_files) | set(final_libomp_files), key=str.casefold)
    add_check(checks, "noLibompFile", not all_libomp_paths, {"paths": all_libomp_paths})
    add_check(checks, "noLibompImport", bool(exe_files) and not dependency_failures and not libomp_imports,
              {"imports": libomp_imports, "unreadableExecutableDependencies": dependency_failures})

    required_exe_imports = {}
    for required_exe in ("fea.exe", "fea-solver.exe"):
        rows = [row for row in executable_reports if Path(row["path"]).name.casefold() == required_exe]
        imports = sorted({name for row in rows for name in row["dependents"]}, key=str.casefold)
        required_exe_imports[required_exe] = {"foundExecutableCount": len(rows), "imports": imports}
    vcomp_imports_ok = all(value["foundExecutableCount"] == 1 and
                           any(name.casefold() == "vcomp140.dll" for name in value["imports"])
                           for value in required_exe_imports.values())
    add_check(checks, "requiredExecutablesImportVcomp140", vcomp_imports_ok, required_exe_imports)

    vcomp_rows = [row for row in inventory if Path(row["path"]).name.casefold() == "vcomp140.dll"]
    redist_digest, redist_size = hash_file(redist_vcomp)
    packaged_matches = (len(vcomp_rows) == 1 and vcomp_rows[0]["sha256"] == redist_digest and
                        vcomp_rows[0]["sizeBytes"] == redist_size)
    add_check(checks, "packagedVcompMatchesSuppliedRedist", packaged_matches,
              {"packagedFiles": vcomp_rows,
               "suppliedRedist": {"path": str(redist_vcomp), "sizeBytes": redist_size,
                                  "sha256": redist_digest}})

    qt_prefix = "doc/licenses/qt-sdk/"
    folded_paths = set(by_relative_path)
    missing_supplier = [name for name in QT_SUPPLIER_NOTICES
                        if qt_prefix + name.casefold() not in folded_paths]
    add_check(checks, "qtSupplierNoticeFiles", not missing_supplier,
              {"expectedCount": len(QT_SUPPLIER_NOTICES), "foundCount": len(QT_SUPPLIER_NOTICES) - len(missing_supplier),
               "missing": missing_supplier})

    attribution_prefix = qt_prefix + "attribution/"
    missing_attributions = [page for page in QT_ATTRIBUTION_PAGES
                            if attribution_prefix + page.casefold() not in folded_paths]
    add_check(checks, "qtAttributionPages", not missing_attributions,
              {"expectedCount": 71, "foundCount": 71 - len(missing_attributions),
               "missing": missing_attributions})

    sbom_prefix = qt_prefix + "sbom/"
    sbom_paths = sorted(path for path in by_relative_path if path.startswith(sbom_prefix))
    add_check(checks, "qtSbomFiles", bool(sbom_paths), {"fileCount": len(sbom_paths), "paths": sbom_paths})

    tetgen_path = "doc/licenses/tetgen/license"
    found_tetgen = by_relative_path.get(tetgen_path)
    add_check(checks, "tetgenLicense", found_tetgen is not None,
              {"expectedPath": "doc/licenses/TetGen/LICENSE", "foundPath": found_tetgen})

    agent_paths = [row["path"] for row in inventory
                   if "agent" in row["path"].casefold() or "range-ai" in row["path"].casefold()]
    agent_documentation_paths = [path for path in agent_paths
                                 if Path(path).suffix.casefold() in AGENT_OFF_DOCUMENTATION_SUFFIXES]
    agent_artifact_paths = [path for path in agent_paths
                            if Path(path).suffix.casefold() in
                            (AGENT_OFF_BINARY_SUFFIXES | AGENT_OFF_UI_OBJECT_SUFFIXES)]
    agent_unclassified_paths = [path for path in agent_paths
                                if path not in agent_documentation_paths and path not in agent_artifact_paths]
    agent_operations_docs = [path for path in agent_documentation_paths
                             if Path(path).name.casefold() == "agent_operations.md"]
    if variant == "OFF":
        add_check(checks, "agentOffInstalledPathAbsence", not agent_artifact_paths,
                  {"variant": variant, "matchingArtifactPaths": agent_artifact_paths,
                   "matchingDocumentationPaths": agent_documentation_paths,
                   "unclassifiedMatchingPaths": agent_unclassified_paths,
                   "scope": "installed executable, library, object, script, and UI/resource path names; embedded strings, linked object contents, and runtime-loaded plugins are not inspected"})
        add_check(checks, "agentOffOperationsDocumentationAbsent", not agent_operations_docs,
                  {"variant": variant, "expectedAbsentFilename": "AGENT_OPERATIONS.md",
                   "presentPaths": agent_operations_docs,
                   "matchingDocumentationPaths": agent_documentation_paths})
    else:
        add_check(checks, "agentOffInstalledPathAbsence", True,
                  {"variant": variant, "status": "NOT_APPLICABLE",
                   "matchingArtifactPaths": agent_artifact_paths,
                   "matchingDocumentationPaths": agent_documentation_paths,
                   "unclassifiedMatchingPaths": agent_unclassified_paths})
        add_check(checks, "agentOffOperationsDocumentationAbsent", True,
                  {"variant": variant, "status": "NOT_APPLICABLE",
                   "expectedAbsentFilename": "AGENT_OPERATIONS.md",
                   "matchingDocumentationPaths": agent_documentation_paths})

    checks_pass = all(check["status"] == "PASS" for check in checks)
    link_map_proof = inspect_fea_link_map(
        link_map_value, build_root, inventory, executable_provenance)
    runtime_plugin_proof = inspect_gui_runtime_receipt(
        gui_runtime_receipt_value, gui_harness_script_value, install_root, inventory)
    release_qualification_status = (
        "INCOMPLETE_LINK_MAP_REVIEW_REQUIRED"
        if link_map_proof["status"] == "SUPPLIED_UNREVIEWED"
        else "INCOMPLETE_LINK_MAP_NOT_SUPPLIED"
    )
    return {
        "schemaVersion": 2,
        "inventoryStatus": "PASS" if checks_pass else "FAIL",
        "releaseQualificationStatus": release_qualification_status,
        "variant": variant,
        "buildRoot": str(build_root),
        "installRoot": str(install_root),
        "fileCount": len(inventory),
        "files": inventory,
        "directories": install_directories,
        "dllFiles": sorted(dll_files, key=lambda row: row["path"].casefold()),
        "executableProvenance": executable_provenance,
        "checks": checks,
        "linkMapProof": link_map_proof,
        "runtimePluginLoadProof": runtime_plugin_proof,
        "legalReview": {"status": "NOT_ASSESSED",
                         "detail": "File presence and hashes are not legal clearance or a deployment-to-license mapping."},
        "evidenceLimit": "Hashes and PE import names describe the scanned bytes and static imports only.",
    }


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", required=True, type=Path,
                        help="Fresh CMake build tree, or its Release configuration directory")
    parser.add_argument("--install-root", required=True, type=Path,
                        help="Clean Windows Release install root to inventory")
    parser.add_argument("--variant", required=True, choices=("ON", "OFF"),
                        help="RANGE_ENABLE_AGENT configure variant")
    parser.add_argument("--redist-vcomp", required=True, type=Path,
                        help="Exact Visual C++ Redist vcomp140.dll supplied to the installer")
    parser.add_argument("--dumpbin", required=True, type=Path,
                        help="Explicit full path to the matching Visual Studio dumpbin.exe")
    parser.add_argument("--supplier-exe", dest="supplier_exe", action="append", nargs=3,
                        metavar=("INSTALL_RELATIVE_PATH", "SOURCE_EXE", "SHA256"),
                        help="Map an installed supplier executable to its source file and expected SHA-256; repeat as needed")
    parser.add_argument("--link-map", type=Path,
                        help="Matching Release fea.map adjacent to the exact Release fea.exe peer")
    parser.add_argument("--gui-runtime-receipt", type=Path,
                        help="Passing installed GUI harness runtime-receipt.json for this exact fea.exe identity")
    parser.add_argument("--gui-harness-script", type=Path,
                        help="Exact harness script named and hash-bound by --gui-runtime-receipt")
    parser.add_argument("--output", required=True, type=Path,
                        help="New JSON receipt path; existing files are never overwritten")
    return parser


def write_report(output_value, report, install_root_value=None):
    output = absolute_path(output_value)
    if output.exists() or output.is_symlink():
        raise AuditError("Refusing to overwrite existing output: {}".format(output))
    if install_root_value is not None:
        install_root = validate_root(install_root_value, "install root")
        prospective_output = output.parent.resolve(strict=False) / output.name
        if is_within(prospective_output, install_root):
            raise AuditError("Output path must be outside the install root")
    output.parent.mkdir(parents=True, exist_ok=True)
    output_parent = output.parent.resolve(strict=True)
    output = output_parent / output.name
    if output.exists() or output.is_symlink():
        raise AuditError("Refusing to overwrite existing output: {}".format(output))
    try:
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
    except OSError as exc:
        raise AuditError("Cannot write output {}: {}".format(output, exc))
    return output


def validate_output_location(output_value, install_root_value):
    output = absolute_path(output_value)
    if output.exists() or output.is_symlink():
        raise AuditError("Refusing to overwrite existing output: {}".format(output))
    install_root = validate_root(install_root_value, "install root")
    prospective_output = output.parent.resolve(strict=False) / output.name
    if is_within(prospective_output, install_root):
        raise AuditError("Output path must be outside the install root")


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    output = absolute_path(args.output)
    if output.exists() or output.is_symlink():
        parser.error("refusing to overwrite existing --output: {}".format(output))
    try:
        validate_output_location(output, args.install_root)
        report = audit_inventory(args.build_root, args.install_root, args.variant,
                                 args.redist_vcomp, args.dumpbin, supplier_exe_mappings=args.supplier_exe,
                                 link_map_value=args.link_map,
                                 gui_runtime_receipt_value=args.gui_runtime_receipt,
                                 gui_harness_script_value=args.gui_harness_script)
    except AuditError as exc:
        report = {
            "schemaVersion": 2,
            "inventoryStatus": "FAIL",
            "releaseQualificationStatus": "INCOMPLETE_AUDIT_ERROR",
            "variant": args.variant,
            "fatalError": str(exc),
            "files": [],
            "directories": [],
            "executableProvenance": [],
            "linkMapProof": {"status": "REJECTED" if args.link_map else "UNAVAILABLE_NOT_SUPPLIED"},
            "runtimePluginLoadProof": {"status": "REJECTED" if args.gui_runtime_receipt else "NOT_MEASURED"},
            "legalReview": {"status": "NOT_ASSESSED"},
        }
        try:
            result_path = write_report(output, report, args.install_root)
        except AuditError as output_exc:
            print("ERROR: {}".format(output_exc), file=sys.stderr)
            return 2
        print("FAIL: {}".format(exc), file=sys.stderr)
        print("Receipt: {}".format(result_path), file=sys.stderr)
        return 1

    try:
        result_path = write_report(output, report, args.install_root)
    except AuditError as exc:
        print("ERROR: {}".format(exc), file=sys.stderr)
        return 2
    print("{}: {}".format(report["inventoryStatus"], result_path))
    print("Release qualification remains {}".format(report["releaseQualificationStatus"]))
    return 0 if report["inventoryStatus"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
