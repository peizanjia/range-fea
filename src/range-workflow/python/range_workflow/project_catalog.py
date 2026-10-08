"""Read the host-owned project catalog; never infer a template is an open document."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path, PurePosixPath
import re
import uuid

from .service import WorkflowError, exact, file_hash

PROTOCOL_VERSION = "range.project/v1"
UNIT_CONVENTION = {"system": "SI", "length": "m", "mass": "kg", "time": "s",
                   "temperature": "K", "angle": "rad", "conversion": False,
                   "numericalAcceptance": False}
MAX_CATALOG_BYTES = 8 * 1024 * 1024
MAX_DOCUMENTS = 4096
MAX_SNAPSHOT_BYTES = 16 * 1024 * 1024 * 1024
_SHA = re.compile(r"^[0-9a-f]{64}$")
_REVISION = re.compile(r"^r(0|[1-9][0-9]*)$")
_DOCUMENT_KEYS = {"persistentDocumentId", "label", "sourcePath", "originDocumentId",
    "sourceRevision", "savedArtifactHash", "dirty", "draftMetadata", "tombstone",
    "modelSha256", "snapshotSourceRevision", "snapshotRelativePath", "snapshotInstanceRef",
    "resolvedSourcePath", "sourceArtifacts", "orphanSnapshots"}


def _fail(message: str):
    raise WorkflowError("PROJECT_CATALOG_INVALID", message)


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            _fail("Duplicate JSON keys in project metadata")
        value[key] = item
    return value


def _bounded_tree(value):
    nodes = 0
    def visit(item, depth):
        nonlocal nodes
        nodes += 1
        if depth > 32 or nodes > 200000:
            _fail("Project metadata exceeds its JSON structure limit")
        if isinstance(item, dict):
            for entry in item.values():
                visit(entry, depth + 1)
        elif isinstance(item, list):
            for entry in item:
                visit(entry, depth + 1)
        elif type(item) is float:
            import math
            if not math.isfinite(item):
                _fail("Project metadata numbers must be finite")
    visit(value, 0)


def _clean_absolute(path):
    return (isinstance(path, str) and bool(path) and Path(path).is_absolute()
            and os.path.normpath(path).replace("\\", "/") == path.replace("\\", "/")
            and _text_bytes(path) <= 4096 and "\x00" not in path)


def _text_bytes(value):
    try:
        return len(value.encode("utf-8"))
    except UnicodeError:
        _fail("Project text must contain valid Unicode")


def _uuid(value, *, empty=False):
    if empty and value == "":
        return
    if not isinstance(value, str):
        _fail("Project identities must be UUID strings")
    try:
        parsed = uuid.UUID(value)
    except (ValueError, AttributeError):
        _fail("Project identities must be UUID strings")
    if parsed.int == 0 or str(parsed) != value:
        _fail("Project identities must be canonical non-null UUIDs")


def _revision(value, *, empty=False):
    if empty and value == "":
        return
    if not isinstance(value, str) or not _REVISION.fullmatch(value) or int(value[1:]) > 2**53 - 1:
        _fail("Project source revisions must be bounded rN tokens")


def _hash(value):
    if not isinstance(value, str) or (value and not _SHA.fullmatch(value)):
        _fail("Project artifact hashes must be SHA-256 strings")


def _draft(value):
    exact(value, set(), {"status", "kind", "changedFields", "fieldCount", "draftId"})
    if "status" in value and value["status"] not in ("none", "pending", "unknown"):
        _fail("Unknown draft status")
    if "kind" in value and value["kind"] not in ("analysis", "selection", "other"):
        _fail("Unknown draft kind")
    if "draftId" in value:
        _uuid(value["draftId"])
    fields = value.get("changedFields", [])
    if not isinstance(fields, list) or len(fields) > 64 or any(
            not isinstance(field, str) or not re.fullmatch(r"[A-Za-z0-9._/-]{1,128}", field)
            for field in fields):
        _fail("Draft field descriptors are invalid")
    if len(set(fields)) != len(fields):
        _fail("Draft field descriptors must be unique")
    if "fieldCount" in value and (type(value["fieldCount"]) is not int or not 0 <= value["fieldCount"] <= 4096):
        _fail("Draft field counts are invalid")


def _instance(value):
    exact(value, {"documentId", "sessionEpoch", "documentRevision"})
    _uuid(value["documentId"])
    _uuid(value["sessionEpoch"])
    revision = value["documentRevision"]
    if type(revision) is not int or not 0 <= revision <= 2**53 - 1:
        _fail("Runtime document revisions are invalid")


def _snapshot_path(workspace: Path, document: dict) -> Path:
    text = document["snapshotRelativePath"]
    expected = f"models/{document['persistentDocumentId']}/snapshots/{document['modelSha256']}/model.rbm"
    if text != expected or "\\" in text or PurePosixPath(text).is_absolute():
        _fail("Snapshot path does not match its document and hash")
    current = workspace
    for part in PurePosixPath(text).parts:
        current = current / part
        if current.is_symlink():
            _fail("Project snapshots must not traverse symbolic links")
    if not current.resolve().is_relative_to(workspace):
        _fail("Project snapshot escaped its workspace")
    return current


def load_project_catalog(workspace: Path) -> dict | None:
    """Validated metadata only. Snapshot byte verification is a separate operation."""
    workspace = Path(workspace).resolve()
    path = workspace / "project.json"
    if not path.exists():
        return None
    if path.is_symlink() or not path.is_file():
        _fail("Project manifest must be a regular workspace file")
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAX_CATALOG_BYTES + 1)
        if len(raw) > MAX_CATALOG_BYTES:
            _fail("Project manifest exceeds its storage limit")
        value = json.loads(raw, object_pairs_hook=_unique_object,
                          parse_constant=lambda _: _fail("Non-finite project metadata"))
    except (OSError, ValueError, UnicodeError, RecursionError):
        _fail("Project manifest could not be read or parsed")
    _bounded_tree(value)
    exact(value, {"storageVersion", "projectId", "documents", "sourcePathIndex"})
    if type(value["storageVersion"]) is not int or value["storageVersion"] != 1:
        raise WorkflowError("PROJECT_VERSION_UNSUPPORTED", "Unsupported project catalog version")
    _uuid(value["projectId"])
    documents, index = value["documents"], value["sourcePathIndex"]
    if not isinstance(documents, dict) or len(documents) > MAX_DOCUMENTS or not isinstance(index, dict):
        _fail("Project document index is invalid")
    expected_index = {}
    for key, document in documents.items():
        _uuid(key)
        exact(document, _DOCUMENT_KEYS)
        if document["persistentDocumentId"] != key:
            _fail("Project document identity does not match its index")
        _uuid(document["originDocumentId"], empty=True)
        if document["originDocumentId"] and document["originDocumentId"] not in documents:
            _fail("Project document origin is unavailable")
        for field in ("label", "sourcePath", "resolvedSourcePath", "snapshotRelativePath"):
            limit = 256 if field == "label" else 4096
            if not isinstance(document[field], str) or _text_bytes(document[field]) > limit or "\x00" in document[field]:
                _fail("Project document text is invalid")
        if not document["label"].strip():
            _fail("Project labels must not be empty")
        if any(ord(character) < 0x20 or ord(character) == 0x7f for character in document["label"]):
            _fail("Project labels must not contain control characters")
        if type(document["dirty"]) is not bool or type(document["tombstone"]) is not bool:
            _fail("Project document flags are invalid")
        _revision(document["sourceRevision"])
        _revision(document["snapshotSourceRevision"], empty=True)
        _hash(document["savedArtifactHash"])
        _hash(document["modelSha256"])
        _draft(document["draftMetadata"])
        source = document["sourcePath"]
        artifacts = document["sourceArtifacts"]
        if not isinstance(artifacts, list) or len(artifacts) > 17:
            _fail("Project source binding exceeds its link depth")
        seen_paths = set()
        for artifact in artifacts:
            exact(artifact, {"path", "sha256"})
            _hash(artifact["sha256"])
            if not _clean_absolute(artifact["path"]) or not artifact["sha256"]:
                _fail("Project source artifact descriptors are invalid")
            alias = artifact["path"].casefold() if os.name == "nt" else artifact["path"]
            if alias in seen_paths:
                _fail("Project source binding contains a cycle")
            seen_paths.add(alias)
            if not document["tombstone"]:
                if alias in expected_index:
                    _fail("Project source aliases are ambiguous")
                expected_index[alias] = key
        if source:
            if (not artifacts or source != artifacts[0]["path"]
                    or document["resolvedSourcePath"] != artifacts[-1]["path"]
                    or document["savedArtifactHash"] != artifacts[-1]["sha256"]):
                _fail("Project source binding is incomplete")
        elif artifacts or document["resolvedSourcePath"] or document["savedArtifactHash"]:
            _fail("Unsaved documents cannot claim a saved source binding")
        if document["modelSha256"]:
            _revision(document["snapshotSourceRevision"])
            _instance(document["snapshotInstanceRef"])
            _snapshot_path(workspace, document)
        elif document["snapshotRelativePath"] or document["snapshotSourceRevision"] or document["snapshotInstanceRef"]:
            _fail("Project snapshot metadata is incomplete")
        orphans = document["orphanSnapshots"]
        if not isinstance(orphans, list) or len(orphans) > 256:
            _fail("Project snapshot history exceeds its limit")
        for orphan in orphans:
            exact(orphan, {"sourceRevision", "modelSha256", "relativePath", "instanceRef"})
            _revision(orphan["sourceRevision"])
            _hash(orphan["modelSha256"])
            if not orphan["modelSha256"]:
                _fail("Archived snapshots must have a content hash")
            _instance(orphan["instanceRef"])
            _snapshot_path(workspace, {"persistentDocumentId": key, "modelSha256": orphan["modelSha256"],
                "snapshotRelativePath": orphan["relativePath"]})
    if index != expected_index:
        _fail("Project source alias index does not match its documents")
    return value


def query_project_catalog(workspace: Path, persistent_document_id: str | None = None) -> dict:
    if persistent_document_id is not None:
        _validate_selector(persistent_document_id)
    value = load_project_catalog(workspace)
    if value is None:
        return {"protocolVersion": PROTOCOL_VERSION, "availability": "unavailable",
                "message": "This workspace has no linked host project catalog."}
    documents = value["documents"]
    if persistent_document_id is not None:
        if persistent_document_id not in documents:
            raise WorkflowError("DOCUMENT_NOT_FOUND", "Unknown persistent document identity")
        documents = {persistent_document_id: documents[persistent_document_id]}
    return {"protocolVersion": PROTOCOL_VERSION, "storageVersion": 1,
            "availability": "available", "projectId": value["projectId"],
            "documents": [copy.deepcopy(documents[key]) for key in sorted(documents)],
            "sourcePathIndex": copy.deepcopy(value["sourcePathIndex"]),
            "unitConvention": copy.deepcopy(UNIT_CONVENTION),
            "snapshotBytesVerified": False}


def _validate_selector(value):
    try:
        _uuid(value)
    except WorkflowError:
        raise WorkflowError("INVALID_PARAMETERS", "persistentDocumentId must be a canonical non-null UUID") from None


def resolve_project_snapshot(workspace: Path, persistent_document_id: str) -> dict:
    _validate_selector(persistent_document_id)
    value = load_project_catalog(workspace)
    if value is None or persistent_document_id not in value["documents"]:
        raise WorkflowError("DOCUMENT_NOT_FOUND", "Unknown persistent document identity")
    document = value["documents"][persistent_document_id]
    if document["tombstone"] or not document["modelSha256"]:
        raise WorkflowError("SNAPSHOT_UNAVAILABLE", "Document has no accepted snapshot")
    if document["snapshotSourceRevision"] != document["sourceRevision"]:
        raise WorkflowError("STALE_SOURCE", "Snapshot does not represent the accepted source revision")
    root = Path(workspace).resolve()
    path = _snapshot_path(root, document)
    try:
        if not path.is_file() or path.stat().st_size > MAX_SNAPSHOT_BYTES:
            raise WorkflowError("SNAPSHOT_UNAVAILABLE", "Snapshot bytes are unavailable or exceed the limit")
        if file_hash(path) != document["modelSha256"]:
            raise WorkflowError("SOURCE_CHANGED", "Accepted project snapshot hash changed")
    except OSError:
        raise WorkflowError("SNAPSHOT_UNAVAILABLE", "Snapshot bytes are unreadable") from None
    return {"protocolVersion": PROTOCOL_VERSION, "projectId": value["projectId"],
            "persistentDocumentId": persistent_document_id,
            "sourceRevision": document["snapshotSourceRevision"],
            "modelSha256": document["modelSha256"], "path": str(path),
            "instanceRef": copy.deepcopy(document["snapshotInstanceRef"]), "snapshotBytesVerified": True}
