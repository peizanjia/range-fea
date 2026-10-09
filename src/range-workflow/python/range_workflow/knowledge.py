"""Strict, packaged operation guidance bound to the live workflow catalog.

This module does not inspect documentation or the filesystem beyond its one
packaged core knowledge file. Runtime descriptors remain the source of truth
for schemas and preconditions; this registry adds human workflow guidance.
"""
from __future__ import annotations

import copy
from datetime import date
import hashlib
import json
import os
from pathlib import Path
import re
import stat
from typing import Any


PACK_SCHEMA = "range.operation-knowledge/v1"
PROTOCOL_VERSION = "range.workflow/v1"
PACKAGE_NAME = "range_workflow"
PACK_RELATIVE_PATH = "knowledge/core-v1.json"
MAX_PACK_BYTES = 256 * 1024
MAX_ITEMS = 5
MAX_QUERY_LENGTH = 256
_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")

# The set of supported groups is intentionally closed. Prefixes are applied
# only after the runtime catalog has supplied an actual operation or command.
_GROUP_PREFIXES = {
    "workflow": ("workflow.",),
    "project": ("project.",),
    "model.source": ("model.source.", "model.sources."),
    "result": ("result.", "results."),
    "native": ("native.",),
    "extension": ("extension.",),
    "task.plan": ("task.plan.",),
    "task.node": ("task.node.",),
    "schema": ("schema.",),
    "knowledge": ("knowledge.",),
    "telemetry": ("telemetry.",),
    "audit": ("audit.",),
    "jobs": ("jobs.", "job."),
    "execution": ("execution.",),
}
_REQUIRED_RULES = {
    ("operation", "capabilities.query"),
    ("operation", "command.preview"),
    ("operation", "command.execute"),
}


class KnowledgeError(ValueError):
    """A knowledge pack or runtime catalog failed closed validation."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def _fail(code: str, message: str) -> None:
    raise KnowledgeError(code, message)


def _canonical(value: Any) -> bytes:
    nodes = 0

    def check(item: Any, depth: int) -> None:
        nonlocal nodes
        nodes += 1
        if depth > 64 or nodes > 200000:
            _fail("KNOWLEDGE_INVALID", "Knowledge or catalog data exceeds its structure limit")
        if isinstance(item, dict):
            if any(not isinstance(key, str) for key in item):
                _fail("KNOWLEDGE_INVALID", "Knowledge object keys must be strings")
            for child in item.values():
                check(child, depth + 1)
        elif isinstance(item, list):
            for child in item:
                check(child, depth + 1)
        elif type(item) is float:
            import math
            if not math.isfinite(item):
                _fail("KNOWLEDGE_INVALID", "Knowledge numbers must be finite")
        elif item is not None and type(item) not in (str, int, bool):
            _fail("KNOWLEDGE_INVALID", "Knowledge data must contain JSON values only")

    try:
        check(value, 0)
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=False, allow_nan=False).encode("utf-8", "strict")
    except (TypeError, ValueError, UnicodeError, RecursionError, OverflowError):
        _fail("KNOWLEDGE_INVALID", "Knowledge or catalog data is not finite JSON")


def _hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            _fail("KNOWLEDGE_INVALID", "The core knowledge pack contains duplicate JSON keys")
        result[key] = value
    return result


def _strict_json(raw: bytes) -> Any:
    try:
        return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=_unique_object,
                          parse_constant=lambda _value: _fail(
                              "KNOWLEDGE_INVALID", "The core knowledge pack contains a non-finite number"))
    except KnowledgeError:
        raise
    except (UnicodeError, ValueError, TypeError, RecursionError):
        _fail("KNOWLEDGE_INVALID", "The core knowledge pack is not valid UTF-8 JSON")


def _exact(value: Any, required: set[str], optional: set[str] = frozenset()) -> dict:
    if (not isinstance(value, dict) or not required <= value.keys() or
            value.keys() - required - optional):
        _fail("KNOWLEDGE_INVALID", "The core knowledge pack has missing or unknown fields")
    return value


def _bounded_tree(value: Any) -> None:
    nodes = 0

    def visit(item: Any, depth: int) -> None:
        nonlocal nodes
        nodes += 1
        if depth > 24 or nodes > 10000:
            _fail("KNOWLEDGE_INVALID", "The core knowledge pack exceeds its structure limit")
        if isinstance(item, dict):
            for key, child in item.items():
                if not isinstance(key, str):
                    _fail("KNOWLEDGE_INVALID", "Knowledge object keys must be strings")
                visit(child, depth + 1)
        elif isinstance(item, list):
            for child in item:
                visit(child, depth + 1)
        elif type(item) is float:
            import math
            if not math.isfinite(item):
                _fail("KNOWLEDGE_INVALID", "Knowledge numbers must be finite")
        elif item is not None and type(item) not in (str, int, bool):
            _fail("KNOWLEDGE_INVALID", "The core knowledge pack must contain JSON values only")

    visit(value, 0)


def _pack_path() -> Path:
    """Return the single permitted packaged file path without following links."""
    module_file = Path(__file__).absolute()
    package_dir = module_file.parent
    knowledge_dir = package_dir / "knowledge"
    path = knowledge_dir / "core-v1.json"
    try:
        if (package_dir.is_symlink() or knowledge_dir.is_symlink() or path.is_symlink() or
                not package_dir.is_dir() or not knowledge_dir.is_dir()):
            _fail("KNOWLEDGE_UNAVAILABLE", "The packaged core knowledge file is unavailable")
        resolved_package = package_dir.resolve(strict=True)
        resolved_directory = knowledge_dir.resolve(strict=True)
        resolved_file = path.resolve(strict=True)
        if (resolved_directory.parent != resolved_package or resolved_file.parent != resolved_directory or
                not stat.S_ISREG(path.stat().st_mode)):
            _fail("KNOWLEDGE_UNAVAILABLE", "The packaged core knowledge file is not a regular local file")
    except KnowledgeError:
        raise
    except (OSError, RuntimeError, ValueError):
        _fail("KNOWLEDGE_UNAVAILABLE", "The packaged core knowledge file is unavailable")
    return path


def _read_pack() -> tuple[dict[str, Any], str, str]:
    path = _pack_path()
    try:
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
        with os.fdopen(descriptor, "rb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                _fail("KNOWLEDGE_UNAVAILABLE", "The packaged core knowledge file is not a regular file")
            raw = stream.read(MAX_PACK_BYTES + 1)
    except KnowledgeError:
        raise
    except OSError:
        _fail("KNOWLEDGE_UNAVAILABLE", "The packaged core knowledge file could not be read")
    if len(raw) > MAX_PACK_BYTES:
        _fail("KNOWLEDGE_INVALID", "The packaged core knowledge file exceeds 256 KiB")
    value = _strict_json(raw)
    _bounded_tree(value)
    pack = _validate_pack(value)
    return pack, _hash(raw), _hash(_canonical(pack))


def _text(value: Any, label: str, limit: int = 4096) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > limit:
        _fail("KNOWLEDGE_INVALID", f"Knowledge {label} is invalid")
    try:
        value.encode("utf-8", "strict")
    except UnicodeError:
        _fail("KNOWLEDGE_INVALID", f"Knowledge {label} is invalid")
    if "\x00" in value:
        _fail("KNOWLEDGE_INVALID", f"Knowledge {label} is invalid")
    return value


def _content(value: Any, *, has_id: bool) -> dict[str, Any]:
    required = {"title", "steps", "success", "recovery"}
    optional: set[str] = set()
    if has_id:
        required |= {"kind", "id"}
        optional.add("requiresApproval")
    _exact(value, required, optional)
    title = _text(value["title"], "title", 160)
    steps = value["steps"]
    if not isinstance(steps, list) or not 1 <= len(steps) <= 8:
        _fail("KNOWLEDGE_INVALID", "Knowledge steps must contain 1..8 entries")
    steps = [_text(step, "step", 512) for step in steps]
    success = _text(value["success"], "success", 1024)
    recovery = _text(value["recovery"], "recovery", 1024)
    result = {"title": title, "steps": steps, "success": success, "recovery": recovery}
    if has_id:
        kind, ident = value["kind"], value["id"]
        if kind != "operation" or not isinstance(ident, str) or not _ID.fullmatch(ident):
            _fail("KNOWLEDGE_INVALID", "Exact rules must identify a supported operation")
        result.update(kind=kind, id=ident)
        if "requiresApproval" in value:
            if type(value["requiresApproval"]) is not bool:
                _fail("KNOWLEDGE_INVALID", "Knowledge approval metadata must be boolean")
            result["requiresApproval"] = value["requiresApproval"]
    return result


def _validate_pack(value: Any) -> dict[str, Any]:
    _exact(value, {"schema", "packId", "version", "date", "status", "protocolVersion",
                   "compatibility", "groups", "rules"})
    if value["schema"] != PACK_SCHEMA:
        _fail("KNOWLEDGE_VERSION_UNSUPPORTED", "Unsupported operation knowledge schema")
    if value["packId"] != "range.workflow.core":
        _fail("KNOWLEDGE_INVALID", "The packaged core knowledge ID is invalid")
    if value["version"] != "1.0.0":
        _fail("KNOWLEDGE_VERSION_UNSUPPORTED", "Unsupported core knowledge pack version")
    if not isinstance(value["date"], str) or not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", value["date"]):
        _fail("KNOWLEDGE_INVALID", "The core knowledge pack date is invalid")
    try:
        date.fromisoformat(value["date"])
    except (TypeError, ValueError):
        _fail("KNOWLEDGE_INVALID", "The core knowledge pack date is invalid")
    if value["status"] != "supported" or value["protocolVersion"] != PROTOCOL_VERSION:
        _fail("KNOWLEDGE_VERSION_UNSUPPORTED", "The core knowledge pack is not supported")

    compatibility = _exact(value["compatibility"], {"protocolVersions"})
    versions = compatibility["protocolVersions"]
    if (not isinstance(versions, list) or not 1 <= len(versions) <= 8 or
            any(not isinstance(item, str) or len(item) > 64 for item in versions) or
            len(set(versions)) != len(versions)):
        _fail("KNOWLEDGE_INVALID", "The knowledge compatibility declaration is invalid")
    if PROTOCOL_VERSION not in versions:
        _fail("KNOWLEDGE_VERSION_UNSUPPORTED", "The core knowledge pack excludes its declared protocol")

    groups_value = value["groups"]
    _exact(groups_value, set(_GROUP_PREFIXES))
    groups = {name: _content(item, has_id=False) for name, item in groups_value.items()}
    rules_value = value["rules"]
    if not isinstance(rules_value, list) or len(rules_value) > 256:
        _fail("KNOWLEDGE_INVALID", "The core knowledge rules are invalid")
    rules: dict[tuple[str, str], dict[str, Any]] = {}
    for raw_rule in rules_value:
        rule = _content(raw_rule, has_id=True)
        key = (rule["kind"], rule["id"])
        if key in rules:
            _fail("KNOWLEDGE_INVALID", "The core knowledge pack contains duplicate exact rules")
        rules[key] = rule
    if not _REQUIRED_RULES <= rules.keys():
        _fail("KNOWLEDGE_INVALID", "The core knowledge pack is missing a required workflow rule")
    return {
        "schema": PACK_SCHEMA,
        "packId": value["packId"],
        "version": value["version"],
        "date": value["date"],
        "status": value["status"],
        "protocolVersion": value["protocolVersion"],
        "compatibility": {"protocolVersions": list(versions)},
        "groups": groups,
        "rules": [rules[key] for key in sorted(rules)],
    }


def load_core_pack() -> dict[str, Any]:
    """Load and validate only ``knowledge/core-v1.json`` beside this module.

    Returns the validated DTO and both the raw-file and semantic canonical
    SHA-256 values. The hashes identify content; they are not signatures.
    """
    pack, raw_sha256, pack_sha256 = _read_pack()
    return {"pack": copy.deepcopy(pack), "rawSha256": raw_sha256,
            "packSha256": pack_sha256, "package": PACKAGE_NAME,
            "sourcePath": f"package:{PACKAGE_NAME}/{PACK_RELATIVE_PATH}"}


def _catalog(capabilities: Any) -> dict[str, Any]:
    if not isinstance(capabilities, dict):
        _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow capabilities must be an object")
    api_version = capabilities.get("apiVersion")
    if not isinstance(api_version, str) or len(api_version) > 64:
        _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow capabilities have no valid protocol version")
    operations, commands = capabilities.get("operations"), capabilities.get("commands")
    if (not isinstance(operations, list) or len(operations) > 4096 or
            not isinstance(commands, list) or len(commands) > 4096):
        _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow operation and command catalogs are invalid")

    def normalize(items: list[Any], kind: str) -> list[dict[str, Any]]:
        found: dict[str, dict[str, Any]] = {}
        for item in items:
            if not isinstance(item, dict):
                _fail("KNOWLEDGE_CATALOG_INVALID", f"Workflow {kind} descriptors must be objects")
            _canonical(item)
            ident = item.get("id")
            if not isinstance(ident, str) or not _ID.fullmatch(ident):
                _fail("KNOWLEDGE_CATALOG_INVALID", f"Workflow {kind} IDs must be bounded identifiers")
            if ident in found:
                _fail("KNOWLEDGE_CATALOG_INVALID", f"Workflow {kind} IDs must be unique")
            read_only = item.get("readOnly", False if kind == "command" else None)
            if type(read_only) is not bool or not isinstance(item.get("parameters"), dict):
                _fail("KNOWLEDGE_CATALOG_INVALID", f"Workflow {kind} descriptor is missing its live schema")
            if kind == "command" and type(item.get("requiresApproval")) is not bool:
                _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow command approval metadata is invalid")
            if "requiresApproval" in item and type(item["requiresApproval"]) is not bool:
                _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow approval metadata is invalid")
            if "trustedOwnerOnly" in item and type(item["trustedOwnerOnly"]) is not bool:
                _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow owner metadata is invalid")
            if "trustedRuntimeRequired" in item and type(item["trustedRuntimeRequired"]) is not bool:
                _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow trusted runtime metadata is invalid")
            normalized = copy.deepcopy(item)
            normalized["readOnly"] = read_only
            found[ident] = normalized
        return [found[key] for key in sorted(found)]

    normalized_operations = normalize(operations, "operation")
    normalized_commands = normalize(commands, "command")
    runtime = capabilities.get("extensionRuntime", {})
    if not isinstance(runtime, dict):
        _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow extension runtime metadata is invalid")
    trusted_extensions = runtime.get("trustedExtensions", False)
    trust_mode = runtime.get("trustMode", "trusted-developer" if trusted_extensions else "disabled")
    os_sandbox = runtime.get("osSandbox", False)
    if (type(trusted_extensions) is not bool or type(os_sandbox) is not bool or
            not isinstance(trust_mode, str) or trust_mode not in {"disabled", "trusted-developer"} or
            (trusted_extensions and trust_mode != "trusted-developer") or
            (not trusted_extensions and trust_mode != "disabled")):
        _fail("KNOWLEDGE_CATALOG_INVALID", "Workflow extension trust flags are inconsistent")
    trust = {"enabled": trusted_extensions, "trustMode": trust_mode, "osSandbox": os_sandbox}
    value = {"protocolVersion": api_version, "operations": normalized_operations,
             "commands": normalized_commands, "trustedRuntime": trust}
    return {"value": value, "operations": normalized_operations, "commands": normalized_commands,
            "trustedRuntime": trust, "catalogSha256": _hash(_canonical(value))}


def _rule_for(pack: dict[str, Any], kind: str, ident: str) -> tuple[dict[str, Any], str]:
    exact = next((rule for rule in pack["rules"]
                  if rule["kind"] == kind and rule["id"] == ident), None)
    if exact is not None:
        return exact, f"rules/{kind}/{ident}"
    for group, prefixes in _GROUP_PREFIXES.items():
        if any(ident.startswith(prefix) for prefix in prefixes):
            return pack["groups"][group], f"groups/{group}"
    _fail("KNOWLEDGE_COVERAGE_INCOMPLETE",
          f"No core knowledge rule covers published {kind} '{ident}'")


def _resolve(pack: dict[str, Any], catalog: dict[str, Any]) -> list[dict[str, Any]]:
    if catalog["value"]["protocolVersion"] not in pack["compatibility"]["protocolVersions"]:
        _fail("KNOWLEDGE_INCOMPATIBLE", "The core knowledge pack does not support this workflow protocol")
    resolved = []
    for kind, key in (("operation", "operations"), ("command", "commands")):
        for descriptor in catalog[key]:
            rule, source_ref = _rule_for(pack, kind, descriptor["id"])
            resolved.append({"kind": kind, "descriptor": descriptor,
                             "rule": rule, "sourceRef": source_ref})
    return resolved


def coverage(capabilities: dict[str, Any]) -> dict[str, Any]:
    """Prove every currently published operation and command has a rule.

    An unknown runtime ID raises ``KNOWLEDGE_COVERAGE_INCOMPLETE``; callers
    cannot receive a partial proof and then treat the missing capability as
    documented.
    """
    loaded = load_core_pack()
    pack = loaded["pack"]
    catalog = _catalog(capabilities)
    resolved = _resolve(pack, catalog)
    return {"complete": True, "packId": pack["packId"], "packVersion": pack["version"],
            "packSha256": loaded["packSha256"], "rawSha256": loaded["rawSha256"],
            "catalogSha256": catalog["catalogSha256"],
            "protocolVersion": catalog["value"]["protocolVersion"],
            "operationIds": [item["descriptor"]["id"] for item in resolved if item["kind"] == "operation"],
            "commandIds": [item["descriptor"]["id"] for item in resolved if item["kind"] == "command"],
            "coveredCount": len(resolved)}


def knowledge_binding(capabilities: dict[str, Any]) -> dict[str, Any]:
    """Return the validated pack and a hash-bound complete live catalog view."""
    loaded = load_core_pack()
    catalog = _catalog(capabilities)
    resolved = _resolve(loaded["pack"], catalog)
    return {"pack": loaded["pack"], "package": loaded["package"],
            "sourcePath": loaded["sourcePath"], "protocolVersion": catalog["value"]["protocolVersion"],
            "rawSha256": loaded["rawSha256"], "packSha256": loaded["packSha256"],
            "catalogSha256": catalog["catalogSha256"],
            "coverage": {"complete": True, "operationCount": len(catalog["operations"]),
                         "commandCount": len(catalog["commands"]), "coveredCount": len(resolved)}}


def _render_text(rule: dict[str, Any]) -> str:
    lines = ["Steps:"]
    lines.extend(f"{index}. {step}" for index, step in enumerate(rule["steps"], 1))
    lines.extend((f"Success: {rule['success']}", f"Recovery: {rule['recovery']}"))
    return "\n".join(lines)


def _runnable_example(kind: str, ident: str) -> str:
    """Return executable client code that uses caller parameters and live IDs only."""
    quoted_id = repr(ident)
    if kind == "command":
        return ("def example(client, parameters, approve):\n"
                f"    preview = client.preview({quoted_id}, parameters)\n"
                "    approval = approve(preview)  # trusted user callback; never auto-approve\n"
                "    receipt = client.execute(preview, approval)\n"
                "    return client.wait(receipt['jobId'])\n")
    return ("def example(client, parameters):\n"
            f"    return client.request({quoted_id}, parameters)\n")


def _prerequisites(kind: str, descriptor: dict[str, Any], schema_hash: str,
                   requires_approval: bool, trusted_runtime: dict[str, Any]) -> list[str]:
    result = [f"Supply parameters from the live schema bound by schemaHash {schema_hash}; this example invents no parameter values."]
    if kind == "command":
        result.append("The required approve callback must obtain separate trusted user approval for the exact preview.")
    elif requires_approval:
        result.append("Complete the required separate trusted approval before calling this operation.")
    if descriptor.get("trustedOwnerOnly", False):
        result.append("Use the trusted owner workflow; the host enforces source registration ownership.")
    if descriptor.get("trustedRuntimeRequired", False):
        result.append("The host must explicitly enable trusted developer mode; this example cannot enable it and the runtime is not an OS sandbox.")
    return result


def _item(loaded: dict[str, Any], catalog: dict[str, Any], resolved: dict[str, Any]) -> dict[str, Any]:
    descriptor, rule, kind = resolved["descriptor"], resolved["rule"], resolved["kind"]
    schema_hash = _hash(_canonical(descriptor["parameters"]))
    trusted_owner = descriptor.get("trustedOwnerOnly", False)
    requires_approval = descriptor.get("requiresApproval", rule.get("requiresApproval", False))
    trusted_runtime = {**catalog["trustedRuntime"],
                       "required": descriptor.get("trustedRuntimeRequired", False)}
    prerequisites = _prerequisites(kind, descriptor, schema_hash, requires_approval, trusted_runtime)
    source_path = f"package:{PACKAGE_NAME}/{PACK_RELATIVE_PATH}#{resolved['sourceRef']}"
    item = {
        "title": rule["title"], "text": _render_text(rule), "sourcePath": source_path,
        "package": PACKAGE_NAME, "protocolVersion": catalog["value"]["protocolVersion"],
        "sha256": loaded["rawSha256"], "packSha256": loaded["packSha256"],
        "catalogSha256": catalog["catalogSha256"], "schemaHash": schema_hash,
        "packId": loaded["pack"]["packId"], "version": loaded["pack"]["version"],
        "date": loaded["pack"]["date"], "status": loaded["pack"]["status"],
        "compatibility": copy.deepcopy(loaded["pack"]["compatibility"]),
        "kind": kind, "id": descriptor["id"],
        "operation": descriptor["id"] if kind == "operation" else None,
        "command": descriptor["id"] if kind == "command" else None,
        "readOnly": descriptor["readOnly"], "requiresApproval": requires_approval,
        "requiresPreparedParameters": True, "prerequisites": prerequisites,
        "trustedOwner": trusted_owner, "trustedOwnerOnly": trusted_owner,
        "trustedRuntime": trusted_runtime,
        "runnableExample": _runnable_example(kind, descriptor["id"]),
    }
    return item


def query_knowledge(capabilities: dict[str, Any], query: str, max_items: int = MAX_ITEMS) -> dict[str, Any]:
    """Search guidance only for entries present in the supplied runtime catalog."""
    if not isinstance(query, str) or len(query) > MAX_QUERY_LENGTH:
        _fail("KNOWLEDGE_QUERY_INVALID", "query must be a string of at most 256 characters")
    try:
        query.encode("utf-8", "strict")
    except UnicodeError:
        _fail("KNOWLEDGE_QUERY_INVALID", "query must contain valid Unicode")
    if type(max_items) is not int or not 1 <= max_items <= MAX_ITEMS:
        _fail("KNOWLEDGE_QUERY_INVALID", "max_items must be an integer in 1..5")
    loaded_pack, raw_sha256, pack_sha256 = _read_pack()
    loaded = {"pack": loaded_pack, "rawSha256": raw_sha256, "packSha256": pack_sha256}
    catalog = _catalog(capabilities)
    resolved = _resolve(loaded_pack, catalog)
    terms = [term.casefold() for term in re.findall(r"[A-Za-z0-9._/-]+|[^\s]+", query) if len(term) > 1]
    ranked = []
    for entry in resolved:
        item = _item(loaded, catalog, entry)
        haystack = (item["id"] + "\n" + item["title"] + "\n" + item["text"]).casefold()
        score = sum(1 for term in terms if term in haystack) if terms else 1
        if score:
            ranked.append((score, item["id"], item))
    ranked.sort(key=lambda row: (-row[0], row[1]))
    return {"items": [row[2] for row in ranked[:max_items]]}
