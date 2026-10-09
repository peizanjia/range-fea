"""Authenticated native manifest reads with bounded origin-table expansion.

This decodes storage only. Callers still verify artifact paths, native records,
and ancestry with the native broker. Origin hashes bind the exact Qt JSON bytes
stored in table strings, never Python's reserialization of a provenance dict.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

from .service import WorkflowError

MAX_MANIFEST_BYTES = 16 * 1024 * 1024
MAX_FRAMES = 10000
MAX_EXPANDED_ORIGIN_BYTES = 128 * 1024 * 1024
ORIGIN_ENCODING = "range.native-origin-table/v1"
_TABLE_KEYS = {"originEncoding", "originTable", "originTableSha256"}


def _sha(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def _bad(message):
    raise WorkflowError("RECOVERY_CONFLICT", message)


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _parse(raw):
    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError("nonfinite JSON number")
        return number
    return json.loads(raw, object_pairs_hook=_unique,
                      parse_float=finite_float,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonfinite JSON")))


def origin_table_sha256(table: dict[str, str]) -> str:
    """Hash sorted SHA keys and length-delimited exact UTF-8 origin JSON."""
    hasher = hashlib.sha256((ORIGIN_ENCODING + "\n").encode("ascii"))
    for key in sorted(table):
        raw = table[key].encode("utf-8")
        hasher.update(key.encode("ascii") + b"\n" + str(len(raw)).encode("ascii") + b":" + raw + b"\n")
    return hasher.hexdigest()


def _no_reference(value, depth=0):
    if depth > 64:
        _bad("Native manifest metadata exceeds its nesting bound")
    if isinstance(value, dict):
        if "originRef" in value:
            _bad("Native manifest has an origin reference outside its declared records")
        for item in value.values():
            _no_reference(item, depth + 1)
    elif isinstance(value, list):
        for item in value:
            _no_reference(item, depth + 1)


def _normalize(manifest):
    if not isinstance(manifest, dict):
        _bad("Native manifest is not an object")
    frames = manifest.get("frames")
    history = manifest.get("reusedHistory", [])
    if (not isinstance(frames, list) or len(frames) > MAX_FRAMES or
            not isinstance(history, list) or len(history) > MAX_FRAMES):
        _bad("Native manifest frame or history index exceeds its bound")
    compact = bool(_TABLE_KEYS & manifest.keys())
    origins, sizes, used = {}, {}, set()
    if compact:
        table = manifest.get("originTable")
        if (manifest.get("originEncoding") != ORIGIN_ENCODING or not isinstance(table, dict) or
                not 1 <= len(table) <= MAX_FRAMES or not _sha(manifest.get("originTableSha256"))):
            _bad("Native manifest origin table is malformed")
        for key, encoded in table.items():
            if not _sha(key) or not isinstance(encoded, str):
                _bad("Native manifest origin table entry is malformed")
            raw = encoded.encode("utf-8")
            if hashlib.sha256(raw).hexdigest() != key:
                _bad("Native manifest origin table entry differs from its original SHA-256")
            value = _parse(raw)
            if not isinstance(value, dict) and value is not None:
                _bad("Native manifest origin must be an object or null")
            # Exact Qt canonical bytes remain the hash scope; Python must not
            # reformat JSON numbers. The broker verifies canonical Qt encoding.
            _no_reference(value)
            origins[key], sizes[key] = value, len(raw)
        if origin_table_sha256(table) != manifest["originTableSha256"]:
            _bad("Native manifest origin table SHA-256 differs")
    expanded_bytes = 0

    def normalize_record(record, *, frame=False):
        nonlocal expanded_bytes
        if not isinstance(record, dict):
            _bad("Native manifest origin-bearing record is malformed")
        if compact:
            key = record.get("originRef")
            if "origin" in record or not isinstance(key, str) or key not in origins:
                _bad("Native manifest record has ambiguous or missing origin references")
            expanded_bytes += sizes[key]
            if expanded_bytes > MAX_EXPANDED_ORIGIN_BYTES:
                _bad("Native manifest expanded origins exceed the 128 MiB budget")
            used.add(key)
            record.pop("originRef")
            record["origin"] = origins[key]
        elif "originRef" in record:
            _bad("Native manifest origin reference has no authenticated table")
        elif "origin" in record:
            _no_reference(record["origin"])
        proof = record.get("historyProof")
        if frame and "historyProof" in record and (compact or proof is not None):
            normalize_record(proof)
        # Check only record metadata; shared origins were checked once above.
        for name, value in record.items():
            if name != "origin" and not (frame and name == "historyProof"):
                _no_reference(value)

    for record in frames:
        normalize_record(record, frame=True)
    for record in history:
        normalize_record(record)
    if compact:
        if len(used) != len(origins):
            _bad("Native manifest origin table has unused entries")
        for key in _TABLE_KEYS:
            manifest.pop(key)
    for key, value in manifest.items():
        if key not in ("frames", "reusedHistory"):
            _no_reference(value)
    return manifest


def decode_native_manifest(raw: bytes, expected_sha256: str) -> dict:
    """Authenticate bounded raw bytes and return the expanded legacy view."""
    if (not isinstance(raw, bytes) or len(raw) > MAX_MANIFEST_BYTES or not _sha(expected_sha256) or
            hashlib.sha256(raw).hexdigest() != expected_sha256):
        _bad("Native manifest bytes are oversized or differ from their expected SHA-256")
    try:
        return _normalize(_parse(raw))
    except (ValueError, TypeError, RecursionError, UnicodeError) as error:
        if isinstance(error, WorkflowError):
            raise
        _bad("Native manifest JSON or origin table is malformed")


def load_native_manifest(path: Path, expected_sha256: str) -> dict:
    """Read exact SHA-bound bytes; callers enforce the workspace path boundary."""
    try:
        path = Path(path)
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_MANIFEST_BYTES:
            _bad("Native manifest must be a bounded regular file")
        with path.open("rb") as stream:
            raw = stream.read(MAX_MANIFEST_BYTES + 1)
        return decode_native_manifest(raw, expected_sha256)
    except OSError:
        _bad("Native manifest is unavailable")
