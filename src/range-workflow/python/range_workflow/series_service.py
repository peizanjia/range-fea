"""Read-only analysis of histories resolved from registered native FE results.

Callers select a successful workflow job, native mode, immutable artifact, and
physical channel.  They cannot supply a series DTO or a filesystem path.
"""
from __future__ import annotations

import copy
import math
import re
import uuid
from typing import Any

from .native_results import query_registered
from .series_analysis import (
    SeriesAnalysisError,
    compare_series,
    envelope_samples,
    phase_plot,
    spectrum,
)
from .service import WorkflowError, canonical, digest, exact, identifier, integer


MAX_SOURCES = 4
MAX_SOURCE_SAMPLES = 10_000
MAX_SOURCE_PAGES = 200
NATIVE_PAGE_SIZE = 200
MAX_DISPLAY_SAMPLES = 1_000
MAX_RESULT_BYTES = 2 * 1024 * 1024
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_REVISION = re.compile(r"^r(0|[1-9][0-9]*)$")
_OPERATIONS = {"envelope", "spectrum", "compare", "phase"}
_PATH_KEY = re.compile(r"path", re.IGNORECASE)

_UPSTREAM_ERRORS = {
    "ARTIFACT_IDENTITY_MISMATCH": ("SOURCE_CHANGED", "Registered native artifact identity changed."),
    "ARTIFACT_PATH": ("NATIVE_RESULTS_UNAVAILABLE", "Registered native source is unavailable."),
    "INVALID_PARAMETERS": ("INVALID_PARAMETERS", "Native source selector is invalid."),
    "INVALID_RESPONSE": ("INVALID_RESPONSE", "Native result broker returned an invalid page."),
    "NATIVE_RESULT_FAILED": ("NATIVE_RESULTS_UNAVAILABLE", "Native result broker could not read the source."),
    "NATIVE_RESULTS_UNAVAILABLE": ("NATIVE_RESULTS_UNAVAILABLE", "Registered native results are unavailable."),
    "RESULT_UNAVAILABLE": ("RESULT_UNAVAILABLE", "A successful registered native result is required."),
    "SOURCE_CHANGED": ("SOURCE_CHANGED", "Registered native source changed during analysis."),
}


def _invalid(message: str = "Series analysis request is invalid.") -> None:
    raise WorkflowError("INVALID_PARAMETERS", message)


def _source_changed() -> None:
    raise WorkflowError("SOURCE_CHANGED", "Registered native source changed during analysis.")


def _checked_query(service: Any, operation: str, parameters: dict) -> dict:
    try:
        result = query_registered(service, operation, parameters)
    except WorkflowError as error:
        code, message = _UPSTREAM_ERRORS.get(
            error.code,
            ("NATIVE_RESULTS_UNAVAILABLE", "Registered native source could not be verified."),
        )
        raise WorkflowError(code, message) from None
    except Exception:
        # Do not return broker, executable, or workspace details to callers.
        raise WorkflowError("NATIVE_RESULTS_UNAVAILABLE",
                            "Registered native source could not be verified.") from None
    if type(result) is not dict:
        raise WorkflowError("INVALID_RESPONSE", "Native result broker returned an invalid page.")
    return result


def _selector(value: Any) -> dict:
    try:
        exact(value, {"jobId", "mode", "artifactId", "channelId"})
    except WorkflowError:
        _invalid("Each source must contain only jobId, mode, artifactId, and channelId.")
    for key in ("jobId", "artifactId", "channelId"):
        if type(value[key]) is not str:
            _invalid("Native source selectors must be bounded identifiers.")
        try:
            identifier(value[key], key)
        except WorkflowError:
            _invalid("Native source selectors must be bounded identifiers.")
    if type(value["mode"]) is not str or value["mode"] not in ("active", "passive"):
        _invalid("Native source mode must be active or passive.")
    return {key: value[key] for key in ("jobId", "mode", "artifactId", "channelId")}


def _model_hash(snapshot_ref: Any) -> str:
    """Select only an authoritative linked model or unlinked input snapshot hash."""
    if type(snapshot_ref) is not dict:
        _source_changed()
    if snapshot_ref.get("kind") == "gui-native-input-snapshot/v1":
        # An unlinked GUI input is identified by its frozen solver-input bytes.
        value = snapshot_ref.get("inputSnapshotSha256")
        if (type(value) is not str or not _SHA256.fullmatch(value) or
                snapshot_ref.get("modelSha256") is not None or
                snapshot_ref.get("inputDerivedFrom") != "live_unlinked" or
                type(snapshot_ref.get("inputPackageSha256")) is not str or
                not _SHA256.fullmatch(snapshot_ref["inputPackageSha256"]) or
                type(snapshot_ref.get("sourceRuntimeStamp")) is not dict):
            _source_changed()
        return value
    if snapshot_ref.get("kind") == "project-native-input-snapshot/v1":
        if (set(snapshot_ref) != {"kind", "projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef"}
                or type(snapshot_ref.get("instanceRef")) is not dict
                or set(snapshot_ref["instanceRef"]) != {"documentId", "sessionEpoch", "documentRevision"}):
            _source_changed()
    # Linked project sources are identified by the catalog's original model
    # hash.  Never substitute a restart/checkpoint or package hash here.
    value = snapshot_ref.get("modelSha256")
    if (type(value) is not str or not _SHA256.fullmatch(value) or
            snapshot_ref.get("kind") not in (None, "", "project-native-input-snapshot/v1") or
            snapshot_ref.get("inputSnapshotSha256") is not None or
            snapshot_ref.get("inputDerivedFrom") is not None):
        _source_changed()
    for key in ("projectId", "persistentDocumentId"):
        raw = snapshot_ref.get(key)
        try:
            if type(raw) is not str or str(uuid.UUID(raw)) != raw:
                _source_changed()
        except (ValueError, AttributeError):
            _source_changed()
    revision = snapshot_ref.get("sourceRevision")
    instance = snapshot_ref.get("instanceRef")
    if (type(revision) is not str or not _REVISION.fullmatch(revision) or
            type(instance) is not dict or
            type(instance.get("documentRevision")) is not int or
            instance["documentRevision"] < 0):
        _source_changed()
    for key in ("documentId", "sessionEpoch"):
        raw = instance.get(key)
        try:
            if type(raw) is not str or str(uuid.UUID(raw)) != raw:
                _source_changed()
        except (ValueError, AttributeError):
            _source_changed()
    return value


def _catalog_pin(catalog: dict, source: dict) -> dict:
    if (catalog.get("jobId") != source["jobId"] or
            catalog.get("mode") != source["mode"] or
            catalog.get("artifactId") != source["artifactId"]):
        _source_changed()
    expected_kind = "native-active-fe" if source["mode"] == "active" else "native-passive-fe"
    if catalog.get("sourceKind") != expected_kind:
        raise WorkflowError("UNSUPPORTED_SOURCE",
                            "The registered native source kind is unsupported by series analysis.")
    manifest_sha = catalog.get("manifestSha256")
    if type(manifest_sha) is not str or not _SHA256.fullmatch(manifest_sha):
        _source_changed()
    snapshot_ref = catalog.get("sourceSnapshotRef")
    try:
        snapshot_copy = copy.deepcopy(snapshot_ref)
        canonical(snapshot_copy)
    except (TypeError, ValueError, RecursionError):
        _source_changed()
    model_sha = _model_hash(snapshot_copy)
    analysis = catalog.get("analysis")
    if (type(analysis) is not dict or analysis.get("domain") != "structural-fe" or
            analysis.get("type") != "structural-dynamics" or
            catalog.get("sampleScope") != "all-persisted-numbered-model-records"):
        raise WorkflowError("UNSUPPORTED_SOURCE",
                            "Only complete native structural-dynamics histories are supported.")
    frame_count = catalog.get("frameCount")
    if type(frame_count) is not int or not 4 <= frame_count <= MAX_SOURCE_SAMPLES:
        raise WorkflowError("SOURCE_LIMIT",
                            "Native history must contain 4..10000 actual records.")
    pinned = {
        "jobId": source["jobId"],
        "mode": source["mode"],
        "artifactId": source["artifactId"],
        "sourceKind": expected_kind,
        "manifestSha256": manifest_sha,
        "sourceSnapshotRef": snapshot_copy,
        "modelSha256": model_sha,
        "analysis": copy.deepcopy(analysis),
        "sampleScope": catalog["sampleScope"],
        "frameCount": frame_count,
    }
    for key in ("validationScope", "proofScope", "sourceRuntimeStamp",
                "inputSnapshotSha256", "inputPackageSha256", "runtime", "control"):
        if key not in catalog:
            _source_changed()
        value = catalog[key]
        if key in ("inputSnapshotSha256", "inputPackageSha256"):
            if type(value) is not str or not _SHA256.fullmatch(value):
                _source_changed()
        elif key in ("validationScope", "proofScope"):
            if type(value) is not str or not value:
                _source_changed()
        elif type(value) is not dict:
            _source_changed()
        try:
            pinned[key] = copy.deepcopy(value)
            canonical(pinned[key])
        except (TypeError, ValueError, RecursionError):
            _source_changed()
    if (snapshot_copy.get("kind") == "gui-native-input-snapshot/v1" and
            (pinned["inputSnapshotSha256"] != model_sha or
             pinned["inputPackageSha256"] != snapshot_copy["inputPackageSha256"])):
        _source_changed()
    return pinned


def _pin_catalog(service: Any, source: dict) -> dict:
    catalog = _checked_query(service, "catalog", {
        "jobId": source["jobId"], "mode": source["mode"],
        "artifactId": source["artifactId"], "limit": 1,
    })
    return _catalog_pin(catalog, source)


def _same_catalog(left: dict, right: dict) -> bool:
    try:
        return canonical(left) == canonical(right)
    except (TypeError, ValueError, RecursionError):
        return False


def _metadata_budget(value: Any, budget: dict[str, int], depth: int = 0) -> None:
    """Bound inert origin/proof JSON before it accumulates across broker pages."""
    budget["nodes"] -= 1
    if budget["nodes"] < 0 or depth > 8:
        raise WorkflowError("SOURCE_LIMIT", "Native origin metadata exceeds its supported limit.")
    if value is None or type(value) is bool:
        return
    if type(value) is str:
        if len(value) > 4096:
            raise WorkflowError("SOURCE_LIMIT", "Native origin metadata exceeds its supported limit.")
        try:
            budget["textBytes"] -= len(value.encode("utf-8", "strict"))
        except UnicodeEncodeError:
            raise WorkflowError("INVALID_RESPONSE", "Native origin metadata is invalid.") from None
        if budget["textBytes"] < 0:
            raise WorkflowError("SOURCE_LIMIT", "Native origin metadata exceeds its supported limit.")
        return
    if type(value) in (int, float):
        try:
            number = float(value)
        except (OverflowError, ValueError):
            number = math.inf
        if not math.isfinite(number) or abs(number) > 1.0e100:
            raise WorkflowError("INVALID_RESPONSE", "Native origin metadata is invalid.")
        return
    if type(value) is list:
        for item in value:
            _metadata_budget(item, budget, depth + 1)
        return
    if type(value) is dict:
        if len(value) > 128:
            raise WorkflowError("SOURCE_LIMIT", "Native origin metadata exceeds its supported limit.")
        for key, item in value.items():
            if type(key) is not str or len(key) > 128:
                raise WorkflowError("INVALID_RESPONSE", "Native origin metadata is invalid.")
            try:
                budget["textBytes"] -= len(key.encode("utf-8", "strict"))
            except UnicodeEncodeError:
                raise WorkflowError("INVALID_RESPONSE", "Native origin metadata is invalid.") from None
            if budget["textBytes"] < 0:
                raise WorkflowError("SOURCE_LIMIT", "Native origin metadata exceeds its supported limit.")
            _metadata_budget(item, budget, depth + 1)
        return
    raise WorkflowError("INVALID_RESPONSE", "Native origin metadata is invalid.")


def _page_identity(page: dict, pin: dict, source: dict) -> dict:
    required = {
        "valid", "operation", "artifactId", "channelId", "nodePhysicalId", "variableId",
        "axis", "unit", "samples", "offset", "limit", "jobId", "mode",
        "completed", "manifestSha256", "sourceKind", "sourceSnapshotRef",
    }
    if (not required <= page.keys() or page.keys() - required - {"nextCursor"} or
            page.get("valid") is not True or
            page.get("operation") != "series" or
            page.get("jobId") != pin["jobId"] or page.get("mode") != pin["mode"] or
            page.get("artifactId") != pin["artifactId"] or
            page.get("channelId") != source["channelId"] or
            page.get("completed") is not True or
            page.get("manifestSha256") != pin["manifestSha256"] or
            page.get("sourceKind") != pin["sourceKind"] or
            page.get("sourceSnapshotRef") != pin["sourceSnapshotRef"]):
        _source_changed()
    node_id = page["nodePhysicalId"]
    try:
        numeric_node_id = float(node_id) if type(node_id) in (int, float) else math.inf
    except (OverflowError, ValueError):
        numeric_node_id = math.inf
    if (not math.isfinite(numeric_node_id) or numeric_node_id < 1 or
            int(numeric_node_id) != numeric_node_id or
            type(page["variableId"]) is not str or not page["variableId"] or
            type(page["axis"]) is not str or not page["axis"] or
            type(page["unit"]) is not str or not page["unit"]):
        raise WorkflowError("INVALID_RESPONSE", "Native channel identity is invalid.")
    if (type(page["samples"]) is not list or len(page["samples"]) > NATIVE_PAGE_SIZE or
            type(page["offset"]) is not int or page["offset"] < 0 or
            type(page["limit"]) is not int or page["limit"] != NATIVE_PAGE_SIZE):
        raise WorkflowError("INVALID_RESPONSE", "Native result broker returned an invalid page.")
    return {key: page[key] for key in ("nodePhysicalId", "variableId", "axis", "unit")}


def _valid_sample(sample: Any, previous_record: int, previous_time: float,
                  budget: dict[str, int]) -> tuple[dict, int, float]:
    if type(sample) is not dict:
        raise WorkflowError("INVALID_RESPONSE", "Native result broker returned an invalid sample.")
    required = {"recordNo", "timeS", "value", "originKind", "origin", "historyProof"}
    if sample.keys() != required:
        raise WorkflowError("INVALID_RESPONSE", "Native result broker returned an invalid sample.")
    record_no = sample["recordNo"]
    time_s, value = sample["timeS"], sample["value"]
    try:
        time_number = float(time_s) if type(time_s) in (int, float) else math.inf
        value_number = float(value) if type(value) in (int, float) else math.inf
    except (OverflowError, ValueError):
        time_number = value_number = math.inf
    if (type(record_no) is not int or not 0 < record_no <= 2**63 - 1 or
            not math.isfinite(time_number) or not math.isfinite(value_number) or
            abs(time_number) > 1.0e100 or abs(value_number) > 1.0e100 or
            record_no <= previous_record or time_number <= previous_time or
            type(sample["originKind"]) is not str or not sample["originKind"]):
        raise WorkflowError("INVALID_RESPONSE", "Native result broker returned an invalid sample.")
    metadata = {
        "originKind": sample["originKind"],
        "origin": sample["origin"],
        "historyProof": sample["historyProof"],
    }
    _metadata_budget(metadata, budget)
    normalized = {
        "time": time_number, "value": value_number, "recordNo": record_no,
        "origin": copy.deepcopy(metadata),
    }
    return normalized, record_no, time_number


def _resolve_source(service: Any, source: dict) -> tuple[dict, dict]:
    pin = _pin_catalog(service, source)
    identity = None
    samples: list[dict] = []
    previous_record, previous_time = 0, -math.inf
    metadata_budget = {"nodes": 200_000, "textBytes": 4 * 1024 * 1024}
    cursor = None
    seen_cursors: set[str] = set()
    pages = 0

    while True:
        if pages >= MAX_SOURCE_PAGES:
            raise WorkflowError("SOURCE_LIMIT", "Native history exceeds the page limit.")
        if cursor is not None:
            if cursor in seen_cursors:
                raise WorkflowError("INVALID_RESPONSE", "Native series cursor repeated.")
            seen_cursors.add(cursor)
        parameters = {
            "jobId": source["jobId"], "mode": source["mode"],
            "artifactId": source["artifactId"], "channelId": source["channelId"],
            "limit": NATIVE_PAGE_SIZE,
        }
        if cursor is not None:
            parameters["cursor"] = cursor
        page = _checked_query(service, "series", parameters)
        page_identity = _page_identity(page, pin, source)
        if identity is None:
            identity = page_identity
        elif page_identity != identity:
            raise WorkflowError("SOURCE_CHANGED", "Native channel identity changed between pages.")

        if len(samples) + len(page["samples"]) > min(MAX_SOURCE_SAMPLES, pin["frameCount"]):
            raise WorkflowError("SOURCE_LIMIT", "Native history exceeds 10000 actual records.")
        if page.get("offset") != len(samples):
            raise WorkflowError("INVALID_RESPONSE", "Native series page offset is inconsistent.")
        for raw_sample in page["samples"]:
            normalized, previous_record, previous_time = _valid_sample(
                raw_sample, previous_record, previous_time, metadata_budget)
            samples.append(normalized)
        pages += 1

        if "nextCursor" not in page:
            break
        next_cursor = page["nextCursor"]
        if (type(next_cursor) is not str or not next_cursor or len(next_cursor) > 4096 or
                next_cursor == cursor or next_cursor in seen_cursors):
            raise WorkflowError("INVALID_RESPONSE", "Native series cursor failed to advance.")
        if not page["samples"]:
            raise WorkflowError("INVALID_RESPONSE", "Native series cursor advanced without records.")
        cursor = next_cursor

    if identity is None or len(samples) != pin["frameCount"]:
        raise WorkflowError("INVALID_RESPONSE", "Native history does not match its registered frame count.")

    # The pure contract names this field `variable`; the native broker reports
    # its stable FE variable ID.  Keep record identity and proof objects inert.
    source_hash = digest({
        "selector": source,
        "sourceKind": pin["sourceKind"],
        "manifestSha256": pin["manifestSha256"],
        "modelSha256": pin["modelSha256"],
        "channel": identity,
        "samples": samples,
    })
    dto = {
        "sourceKind": pin["sourceKind"],
        "jobId": source["jobId"],
        "artifactId": source["artifactId"],
        "modelSha256": pin["modelSha256"],
        "channelId": source["channelId"],
        "axis": identity["axis"],
        "variable": identity["variableId"],
        "unit": identity["unit"],
        "timeUnit": "s",
        "completed": True,
        "sourceHash": source_hash,
        "samples": samples,
    }

    # query_registered rechecks job/artifact/manifest on every operation; this
    # final catalog pin closes the extraction window across all series pages.
    final_pin = _pin_catalog(service, source)
    if not _same_catalog(pin, final_pin):
        _source_changed()
    return dto, pin


def _public_value(value: Any) -> Any:
    """Copy a result while removing all path-named fields recursively."""
    if type(value) is dict:
        return {key: _public_value(item) for key, item in value.items()
                if not _PATH_KEY.search(key)}
    if type(value) is list:
        return [_public_value(item) for item in value]
    return value


def _parse_request(parameters: Any) -> tuple[str, list[dict], dict]:
    try:
        exact(parameters, {"operation", "sources", "options"})
    except WorkflowError:
        _invalid("Request must contain only operation, sources, and options.")
    operation = parameters["operation"]
    if type(operation) is not str or operation not in _OPERATIONS:
        _invalid("Unsupported series analysis operation.")
    raw_sources = parameters["sources"]
    expected_count = 1 if operation in ("envelope", "spectrum") else 2
    if (type(raw_sources) is not list or not 1 <= len(raw_sources) <= MAX_SOURCES or
            len(raw_sources) != expected_count):
        _invalid("Operation has an invalid number of native sources.")
    sources = [_selector(value) for value in raw_sources]
    options = parameters["options"]
    if type(options) is not dict:
        _invalid("Operation options must be an object.")
    allowed = {
        "envelope": {"limit"},
        "spectrum": {"window", "removeMean"},
        "compare": set(),
        "phase": set(),
    }[operation]
    try:
        exact(options, set(), allowed)
    except WorkflowError:
        _invalid("Options contain fields unsupported by this operation.")
    normalized = {}
    if operation == "envelope":
        limit = options.get("limit", MAX_DISPLAY_SAMPLES)
        try:
            normalized["limit"] = integer(limit, "limit", 8, MAX_DISPLAY_SAMPLES)
        except WorkflowError:
            _invalid("Envelope limit must be an integer in 8..1000.")
    elif operation == "spectrum":
        window = options.get("window", "hann")
        remove_mean = options.get("removeMean", True)
        if type(window) is not str or window not in ("hann", "rectangular"):
            _invalid("Spectrum window must be hann or rectangular.")
        if type(remove_mean) is not bool:
            _invalid("removeMean must be a boolean.")
        normalized.update(window=window, remove_mean=remove_mean)
    return operation, sources, normalized


def handle(service: Any, parameters: Any) -> dict:
    """Resolve and analyze registered native histories without modifying state.

    Public request:
      {"operation": "envelope|spectrum|compare|phase",
       "sources": [{"jobId", "mode", "artifactId", "channelId"}, ...],
       "options": {...}}
    """
    operation, selectors, options = _parse_request(parameters)
    resolved = [_resolve_source(service, selector) for selector in selectors]
    dto = [item[0] for item in resolved]
    try:
        if operation == "envelope":
            result = envelope_samples(dto[0], limit=options["limit"])
        elif operation == "spectrum":
            result = spectrum(dto[0], window=options["window"],
                              remove_mean=options["remove_mean"])
        elif operation == "compare":
            result = compare_series(dto[0], dto[1])
        else:
            result = phase_plot(dto[0], dto[1])
    except SeriesAnalysisError:
        raise WorkflowError("INVALID_SERIES", "Native histories do not satisfy the requested analysis.") from None
    public = _public_value(result)
    try:
        serialized = canonical(public)
    except (TypeError, ValueError, RecursionError):
        raise WorkflowError("INVALID_RESPONSE", "Series analysis produced an invalid result.") from None
    if len(serialized) > MAX_RESULT_BYTES:
        raise WorkflowError("OUTPUT_LIMIT", "Series analysis result exceeds its 2 MiB limit.")
    return public


__all__ = ["handle"]
