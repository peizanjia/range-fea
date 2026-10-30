"""Authenticated, bounded WF10 histories. Caller DTOs and paths never enter here."""
from __future__ import annotations

import copy
import json
import math
from pathlib import Path
import re

import numpy as np

from .native_results import query_registered
from .series_analysis import validate_series, native_piezo_channel
from .series_service import _model_hash as _linked_model_hash
from .service import WorkflowError, canonical, digest, exact, file_hash, identifier

MAX_SAMPLES = 10_000
MAX_BYTES = 32 * 1024 * 1024
_SHA = re.compile(r"^[0-9a-f]{64}$")
_CHANNEL = re.compile(r"^node:([1-9][0-9]{0,9}):(displacement|velocity|acceleration|rotation|angularVelocity|angularAcceleration):(X|Y|Z)$")
_PHYSICAL = {
    "displacement": ("var-displacement", "m"), "velocity": ("var-velocity", "m/s"),
    "acceleration": ("var-acceleration", "m/s^2"), "rotation": ("var-rotation", "rad"),
    "angularVelocity": ("var-angular_velocity", "rad/s"),
    "angularAcceleration": ("var-angular_acceleration", "rad/s^2"),
}


def catalog(service, parameters):
    exact(parameters, {"jobId"}, {"mode"})
    job_id = identifier(parameters["jobId"], "jobId")
    mode = parameters.get("mode", "active")
    if mode not in {"active", "passive"}:
        fail("INVALID_PARAMETERS", "Native mode must be active or passive")
    pin = query_registered(service, "catalog", {"jobId": job_id, "mode": mode, "limit": 1})
    channels, cursor = [], None
    while True:
        page = query_registered(service, "channels", {"jobId": job_id, "mode": mode,
            "artifactId": pin["artifactId"], "limit": 200, **({"cursor": cursor} if cursor else {})})
        for channel in page["channels"]:
            chosen = {"kind": "native", "jobId": job_id, "mode": mode,
                      "artifactId": pin["artifactId"], "channelId": channel["channelId"]}
            channels.append({"selector": chosen, "label": channel["channelId"],
                "unit": channel["unit"], "quantity": channel.get("quantity", channel["variableId"])})
        next_cursor = page.get("nextCursor")
        if not next_cursor:
            break
        if next_cursor == cursor or len(channels) >= 2048:
            fail("SOURCE_LIMIT", "Native channel catalog exceeds the workbench bound")
        cursor = next_cursor
    unavailable = []
    if channels and mode == "active":
        try:
            reference = reference_identity(service, channels[0]["selector"])
            matched = False
            for channel in channels:
                if channel["selector"]["channelId"] == reference["channelId"]:
                    channel["reference"] = reference["selector"]
                    matched = True
            if not matched:
                fail("REFERENCE_UNAVAILABLE", "Configured reference sensor is absent from native channels")
        except WorkflowError as error:
            unavailable.append({"kind": "reference", "code": error.code, "message": error.message})
    job = _job(service, job_id)
    checked = job["result"].get("sourceReference")
    derived = []
    if type(checked) is dict and checked.get("matrixComparison") is not None:
        derived.append({"selector": {"kind": "energy", "jobId": job_id, "mode": mode,
                                     "artifactId": pin["artifactId"]},
                        "label": "Mechanical energy derived from actual FE q/v", "unit": "J"})
    return {"schemaId": "range.series-source-catalog/v1", "jobId": job_id, "mode": mode,
            "artifactId": pin["artifactId"], "channels": channels, "derived": derived,
            "unavailable": unavailable}


def fail(code, message):
    raise WorkflowError(code, message)


def sha(value):
    if type(value) is not str or not _SHA.fullmatch(value):
        fail("INVALID_RESPONSE", "Source identity must be a lowercase SHA-256 digest.")
    return value


def finite(value):
    try:
        result = float(value) if type(value) in (int, float) else math.inf
    except (OverflowError, ValueError):
        result = math.inf
    if not math.isfinite(result) or abs(result) > 1e100:
        fail("INVALID_RESPONSE", "Source contains an invalid finite number.")
    return result


def selector(value):
    if type(value) is dict and value.get("kind") == "energy":
        exact(value, {"kind", "jobId", "mode", "artifactId"}, {"sourceHash"})
    else:
        exact(value, {"kind", "jobId", "mode", "artifactId", "channelId"},
              {"referenceSha256", "sourceHash"})
    result = copy.deepcopy(value)
    for key in ("jobId", "artifactId", "channelId"):
        if key not in result:
            continue
        if type(result[key]) is not str:
            fail("INVALID_PARAMETERS", "Source selectors must contain bounded identifiers.")
        if key != "channelId" or native_piezo_channel(result[key]) is None:
            identifier(result[key], key)
    if result["kind"] not in ("native", "reference", "energy") or result["mode"] not in ("active", "passive"):
        fail("INVALID_PARAMETERS", "Source kind or native mode is invalid.")
    if result["kind"] == "reference":
        if result["mode"] != "active" or "referenceSha256" not in result:
            fail("INVALID_PARAMETERS", "Independent reference requires active mode and its identity.")
        if type(result["referenceSha256"]) is not str or not _SHA.fullmatch(result["referenceSha256"]):
            fail("INVALID_PARAMETERS", "Reference identity must be a lowercase SHA-256 digest.")
    elif "referenceSha256" in result:
        fail("INVALID_PARAMETERS", "Native sources cannot carry a reference identity.")
    if "sourceHash" in result:
        if type(result["sourceHash"]) is not str or not _SHA.fullmatch(result["sourceHash"]):
            fail("INVALID_PARAMETERS", "Held source identity must be a lowercase SHA-256 digest.")
    is_work = result["kind"] == "native" and result.get("channelId") == "control:work"
    if is_work and result["mode"] != "active":
        fail("CHANNEL_UNAVAILABLE", "Passive results have no accepted controller work ledger")
    if result["kind"] != "energy" and not is_work and not _CHANNEL.fullmatch(result["channelId"]) and not (result["kind"] == "native" and native_piezo_channel(result["channelId"])):
        fail("INVALID_PARAMETERS", "Select a persisted physical native channel.")
    return result


def _safe_reference(value, depth=0, budget=None):
    if budget is None:
        budget = [200_000, 4 * 1024 * 1024]
    budget[0] -= 1
    if budget[0] < 0 or depth > 8:
        fail("SOURCE_LIMIT", "Source metadata exceeds the supported budget.")
    if type(value) is dict:
        if len(value) > 128:
            fail("SOURCE_LIMIT", "Source metadata exceeds the supported budget.")
        for key, item in value.items():
            if type(key) is not str or len(key) > 128 or "path" in key.lower():
                fail("INVALID_RESPONSE", "Source metadata cannot contain filesystem paths.")
            _safe_reference(key, depth + 1, budget)
            _safe_reference(item, depth + 1, budget)
    elif type(value) is list:
        for item in value:
            _safe_reference(item, depth + 1, budget)
    elif type(value) is str:
        if (len(value) > 4096 or "\\" in value or re.match(r"^[A-Za-z]:", value)
                or value.startswith(("/", "file:", "./", "../"))
                or ("/" in value and not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]*/v[1-9][0-9]*", value))
                or re.fullmatch(r"[^/\\]+\.[A-Za-z0-9]{1,16}", value)):
            fail("INVALID_RESPONSE", "Source metadata cannot contain filesystem paths.")
        try:
            budget[1] -= len(value.encode("utf-8", "strict"))
        except UnicodeEncodeError:
            fail("INVALID_RESPONSE", "Source metadata has invalid text.")
        if budget[1] < 0:
            fail("SOURCE_LIMIT", "Source metadata exceeds the supported budget.")
    elif value is None or type(value) is bool:
        pass
    elif type(value) in (int, float):
        finite(value)
    else:
        fail("INVALID_RESPONSE", "Source metadata must be inert JSON.")


def _query(service, operation, parameters):
    try:
        result = query_registered(service, operation, parameters)
    except WorkflowError:
        raise
    except Exception:
        fail("NATIVE_RESULTS_UNAVAILABLE", "Registered native source could not be verified.")
    if type(result) is not dict:
        fail("INVALID_RESPONSE", "Native broker returned an invalid object.")
    return result


def _model_identity(ref):
    if type(ref) is not dict:
        fail("INVALID_RESPONSE", "Native source reference is invalid.")
    _safe_reference(ref)
    if ref.get("kind") == "gui-native-input-snapshot/v1":
        if ref.get("inputDerivedFrom") != "live_unlinked" or ref.get("modelSha256") is not None:
            fail("INVALID_RESPONSE", "Unlinked input source identity is inconsistent.")
        sha(ref.get("inputPackageSha256"))
        return sha(ref.get("inputSnapshotSha256"))
    if ref.get("kind") == "template":
        if ref.get("usesOpenDocument") is not False:
            fail("INVALID_RESPONSE", "Template source identity is inconsistent.")
        return sha(ref.get("sourceSha256"))
    return _linked_model_hash(ref)


def _catalog_pin(service, source):
    parameters = {k: source[k] for k in ("jobId", "mode", "artifactId")}
    catalog = _query(service, "catalog", {**parameters, "limit": 1})
    kind = "native-" + source["mode"] + "-fe"
    if (any(catalog.get(k) != v for k, v in parameters.items()) or
            catalog.get("sourceKind") != kind or type(catalog.get("analysis")) is not dict or
            catalog["analysis"].get("domain") != "structural-fe" or
            catalog["analysis"].get("type") != "structural-dynamics" or
            catalog.get("sampleScope") != "all-persisted-numbered-model-records"):
        fail("UNSUPPORTED_SOURCE", "Only registered complete structural dynamic histories are supported.")
    count = catalog.get("frameCount")
    if type(count) is not int or not 4 <= count <= MAX_SAMPLES:
        fail("SOURCE_LIMIT", "Complete histories require 4..10000 persisted records.")
    proof = catalog.get("proofScope")
    if type(proof) is not str or not proof or len(proof) > 128:
        fail("INVALID_RESPONSE", "Native proof scope is invalid.")
    ref = copy.deepcopy(catalog.get("sourceSnapshotRef"))
    accepted = {}
    if "nativeAcceptedIdentity" in catalog:
        accepted["nativeAcceptedIdentity"] = _accepted_identity(catalog["nativeAcceptedIdentity"])
    return {**parameters, **accepted, "sourceKind": kind, "manifestSha256": sha(catalog.get("manifestSha256")),
            "modelSha256": _model_identity(ref), "sourceSnapshotRef": ref,
            "proofScope": proof, "frameCount": count}


def _accepted_identity(value):
    fields = {"family", "sourceFingerprint", "configurationIdentity", "backendIdentity", "backendControlSha256",
              "targetSteps", "dtS", "initialTimeS", "identityScope", "electricalWorkSource"}
    if (type(value) is not dict or set(value) != fields or
            value.get("identityScope") != "compatibility-not-authentication" or
            type(value.get("targetSteps")) is not int or not 1 <= value["targetSteps"] <= 2**63 - 1 or finite(value.get("dtS")) <= 0):
        fail("INVALID_RESPONSE", "Native accepted identity is invalid")
    finite(value["initialTimeS"])
    if any(type(value[k]) is not str or not value[k] or len(value[k]) > 4096 for k in fields - {"targetSteps", "dtS", "initialTimeS"}):
        fail("INVALID_RESPONSE", "Native accepted identity text is invalid")
    if value["family"] not in {"axial", "layered"}:
        fail("INVALID_RESPONSE", "Native accepted family is invalid")
    for key in ("sourceFingerprint", "configurationIdentity", "backendControlSha256"):
        sha(value[key])
    backend = value["backendIdentity"]
    native_backend = (type(backend) is str and re.fullmatch(
        r"range-fea/native-(?:python-(?:axial|layered)/v1:[0-9a-f]{64}|(?:axial|layered)-Newmark/no-control/v1)", backend))
    # These are backend identity tokens emitted by the native facades, not paths.
    if not native_backend:
        _safe_reference(backend)
    _safe_reference({k: v for k, v in value.items() if k != "backendIdentity"})
    return copy.deepcopy(value)


def _coverage(value, frames):
    continuous = all(b["recordNo"] == a["recordNo"] + 1 for a, b in zip(frames, frames[1:]))
    expected = {"coverage": "exact selected certified accepted records; no interpolation or claim of complete FE history",
                "sampledCoverage": True, "continuousBetweenSelectedRecords": continuous}
    if type(value) is not dict or value != expected or any(type(value.get(k)) is not bool for k in ("sampledCoverage", "continuousBetweenSelectedRecords")):
        fail("INVALID_RESPONSE", "Native accepted coverage is inconsistent")
    return copy.deepcopy(value)


def _piezo_metadata(channel_id, metadata):
    quantity, axis = native_piezo_channel(channel_id)
    units = {"piezoReadout": {"V", "C"}, "electrodeVoltage": {"V"}, "positiveElectrodeCharge": {"C"},
             "idealAmplifierVoltage": {"V"}, "voltageAction": {"V"}, "electricalWork": {"J"}}
    targets = ({"accepted-piezo-action"} if quantity == "voltageAction" else
               {"accepted-piezo-electrical-ledger"} if quantity == "electricalWork" else
               {"accepted-named-piezo-port", "accepted-piezo-action-record"} if quantity in {"piezoReadout", "electrodeVoltage"} else
               {"accepted-named-piezo-port"})
    if (metadata["channelId"] != channel_id or metadata["quantity"] != quantity or metadata["axis"] != axis or
            metadata["variableId"] != "native-" + quantity or metadata["unit"] not in units[quantity] or metadata["nodeTarget"] not in targets):
        fail("INVALID_RESPONSE", "Native piezo channel identity or SI unit is inconsistent")
    return copy.deepcopy(metadata)


def _snapshot(service, sources):
    source = sources[0]
    pin = _catalog_pin(service, source)
    ids = list(dict.fromkeys(s["channelId"] for s in sources))
    if not 1 <= len(ids) <= 16:
        fail("SOURCE_LIMIT", "A snapshot supports at most 16 physical channels.")
    parameters = {k: source[k] for k in ("jobId", "mode", "artifactId")}
    value = _query(service, "snapshot", {**parameters, "channelIds": ids})
    required = {"valid", "operation", "apiVersion", "schemaId", "completed", "live", "artifactId",
                "jobId", "sourceKind", "sourceSnapshotRef", "proofScope", "manifestSha256",
                "frameCount", "channels", "frames"}
    if (not required <= value.keys() or value.keys() - required - {"mode", "nativeAcceptedCoverage"} or
            value.get("valid") is not True or value.get("completed") is not True or
            value.get("live") is not False or value.get("operation") != "snapshot" or
            value.get("apiVersion") != "range.native-series-snapshot/v1" or
            value.get("schemaId") != "range.native-series-snapshot/v1" or
            any(value.get(k) != pin[k] for k in ("jobId", "artifactId", "sourceKind",
                "manifestSha256", "sourceSnapshotRef", "proofScope", "frameCount")) or
            ("mode" in value and value["mode"] != source["mode"])):
        fail("SOURCE_CHANGED", "Native snapshot identity differs from its registered source.")
    try:
        byte_count = len(canonical(value))
    except (TypeError, ValueError, RecursionError):
        fail("INVALID_RESPONSE", "Native snapshot must contain bounded finite JSON.")
    if byte_count > MAX_BYTES:
        fail("SOURCE_LIMIT", "Native snapshot exceeds 32 MiB.")
    channels = value["channels"]
    if type(channels) is not list or len(channels) != len(ids):
        fail("INVALID_RESPONSE", "Native snapshot channels are inconsistent.")
    identities = {}
    for channel_id, metadata in zip(ids, channels):
        if (type(metadata) is not dict or set(metadata) != {"channelId", "variableId", "quantity", "axis", "unit", "nodeTarget"}
                or any(type(v) is not str for v in metadata.values())):
            fail("INVALID_RESPONSE", "Native snapshot channel metadata is invalid.")
        if channel_id == "control:work":
            if source["mode"] != "active" or metadata != {"channelId": "control:work",
                    "variableId": "control-cumulative-work", "quantity": "controlWork", "axis": "scalar",
                    "unit": "J", "nodeTarget": "accepted-control-ledger"}:
                fail("INVALID_RESPONSE", "Accepted control work identity or physical unit is inconsistent")
            identities[channel_id] = copy.deepcopy(metadata)
            continue
        if native_piezo_channel(channel_id):
            if "nativeAcceptedIdentity" not in pin:
                fail("INVALID_RESPONSE", "Piezo channel has no certified accepted identity")
            identities[channel_id] = _piezo_metadata(channel_id, metadata)
            continue
        match = _CHANNEL.fullmatch(channel_id)
        node, quantity, axis = match.groups()
        variable, unit = _PHYSICAL[quantity]
        if metadata != {"channelId": channel_id, "variableId": variable, "quantity": quantity,
                        "axis": axis, "unit": unit, "nodeTarget": ("physical-component-of-supported-native-kinematics" if "nativeAcceptedIdentity" in pin else "persistent-node-physical-id")}:
            fail("INVALID_RESPONSE", "Native channel identity or physical unit is inconsistent.")
        identities[channel_id] = {**metadata, "nodePhysicalId": int(node)}
    frames = value["frames"]
    if type(frames) is not list or len(frames) != pin["frameCount"]:
        fail("INVALID_RESPONSE", "Native snapshot record count is inconsistent.")
    previous_record, previous_time = 0, -math.inf
    normalized = []
    for frame in frames:
        if type(frame) is not dict or set(frame) != {"recordNo", "timeS", "modelSha256", "originKind", "originSha256", "historyProofSha256", "values"}:
            fail("INVALID_RESPONSE", "Native snapshot frame is invalid.")
        record, time = frame["recordNo"], finite(frame["timeS"])
        if (type(record) is not int or not previous_record < record <= 2**63 - 1 or
                not previous_time < time or type(frame["originKind"]) is not str or
                not re.fullmatch(r"[A-Za-z0-9._:-]{1,128}", frame["originKind"]) or
                type(frame["values"]) is not list or len(frame["values"]) != len(ids)):
            fail("INVALID_RESPONSE", "Native frame order, time, or value shape is invalid.")
        for key in ("modelSha256", "originSha256", "historyProofSha256"):
            sha(frame[key])
        normalized.append({**frame, "timeS": time, "values": [finite(v) for v in frame["values"]]})
        previous_record, previous_time = record, time
    if "nativeAcceptedIdentity" in pin:
        pin["nativeAcceptedCoverage"] = _coverage(value.get("nativeAcceptedCoverage"), normalized)
    elif "nativeAcceptedCoverage" in value:
        fail("INVALID_RESPONSE", "Accepted coverage has no certified identity")
    comparison_pin = {k: v for k, v in pin.items() if k != "nativeAcceptedCoverage"}
    if _catalog_pin(service, source) != comparison_pin:
        fail("SOURCE_CHANGED", "Native source changed during extraction.")
    return pin, identities, normalized


def _dto(source, pin, identity, samples, origin):
    bare = {k: v for k, v in source.items() if k != "sourceHash"}
    kind = "independent-reference" if source["kind"] == "reference" else pin["sourceKind"]
    source_hash = digest({"selector": bare, "pin": pin, "channel": identity,
                          "origin": origin, "samples": samples})
    if "sourceHash" in source and source["sourceHash"] != source_hash:
        fail("SOURCE_CHANGED", "Source differs from its held display identity.")
    dto = {"sourceKind": kind, "jobId": source["jobId"], "artifactId": source["artifactId"],
           "modelSha256": pin["modelSha256"], "channelId": identity["channelId"],
           "axis": identity["axis"], "variable": identity["variableId"], "unit": identity["unit"],
           "timeUnit": "s", "completed": True, "sourceHash": source_hash,
           "origin": origin, "samples": samples}
    dto = validate_series(dto)
    metadata = {"selector": {**bare, "sourceHash": source_hash}, "sourceKind": kind,
                "modelSha256": pin["modelSha256"], "manifestSha256": pin["manifestSha256"],
                "sourceHash": source_hash, "rawSampleCount": len(samples),
                "channel": identity, "proofScope": origin.get("proofScope", pin["proofScope"])}
    if source["kind"] == "reference":
        metadata["referenceSha256"] = source["referenceSha256"]
    return {"dto": dto, "metadata": metadata, "pin": pin}


def _job(service, job_id):
    job = service._job(job_id)
    if job.get("state") != "succeeded":
        fail("RESULT_UNAVAILABLE", "A successful registered job is required.")
    return job


def _bound_json(service, job, key):
    """Authenticate original source files to both result and owning job ledger."""
    path = Path(job.get("result", {}).get(key, ""))
    try:
        root = service.workspace / "jobs"
        if not path.is_absolute():
            raise ValueError()
        relative = path.relative_to(root)
        owner = _job(service, relative.parts[0])
        if owner["jobId"] != job["jobId"]:
            bindings = job.get("result", {}).get("sourceArtifacts", {}).values()
            if not any(type(a) is dict and a.get("path") == str(path) for a in bindings):
                raise ValueError()
        cursor = root
        if cursor.is_symlink():
            raise ValueError()
        for component in relative.parts:
            cursor /= component
            if cursor.is_symlink():
                raise ValueError()
        path.resolve(strict=True).relative_to(root.resolve(strict=True))
        entries = [a for a in owner.get("artifacts", []) if a.get("path") == str(path)]
        if len(entries) != 1 or not path.is_file() or path.stat().st_size > MAX_BYTES:
            raise ValueError()
        expected = sha(entries[0].get("sha256"))
        if owner["jobId"] != job["jobId"] and not any(
                type(a) is dict and a.get("path") == str(path) and a.get("sha256") == expected
                for a in job["result"]["sourceArtifacts"].values()):
            raise ValueError()
        if file_hash(path) != expected:
            raise ValueError()
        def unique(pairs):
            out = {}
            for name, item in pairs:
                if name in out:
                    raise ValueError()
                out[name] = item
            return out
        data = json.loads(path.read_bytes(), object_pairs_hook=unique,
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
        canonical(data)
        if type(data) is not dict or file_hash(path) != expected:
            raise ValueError()
        return data, expected
    except (OSError, ValueError, TypeError, IndexError, RecursionError):
        fail("SOURCE_CHANGED", "Registered independent source artifact could not be verified.")


def _reference_material(service, source):
    job = _job(service, source["jobId"])
    reference, reference_sha = _bound_json(service, job, "referencePath")
    linear, linear_sha = _bound_json(service, job, "linearPath")
    gains, gains_sha = _bound_json(service, job, "gainsPath")
    runtime = job["result"].get("runtimeReference")
    identity = reference_sha
    if runtime is not None:
        from .runtime_reference import accepted_schedule
        try:
            schedule = accepted_schedule(job["result"]["runtimeParameters"],
                                         job["result"]["runtimeProposalRecords"])
        except (ValueError, KeyError, TypeError):
            fail("SOURCE_CHANGED", "Accepted reference schedule is unavailable or inconsistent.")
        identity = digest({"runtimeReference": runtime, "acceptedSchedule": schedule,
                           "referenceSha256": reference_sha, "linearSha256": linear_sha,
                           "gainsSha256": gains_sha})
    return job, reference, linear, gains, runtime, identity


def _sensor_identity(linear, gains, reference):
    configured = gains.get("sensor")
    if type(configured) is not dict or type(configured.get("id")) is not str:
        fail("REFERENCE_UNAVAILABLE", "Independent source lacks its configured controller sensor.")
    sensor_id = configured["id"]
    if reference.get("sensorId") != sensor_id:
        fail("REFERENCE_UNAVAILABLE", "Controller and independent reference sensor identities differ.")
    sensors = linear.get("outputs")
    if type(sensors) is not list:
        fail("REFERENCE_UNAVAILABLE", "Independent source has no physical sensor catalog.")
    matches = [(i, s) for i, s in enumerate(sensors) if type(s) is dict and s.get("id") == sensor_id]
    if len(matches) != 1:
        fail("REFERENCE_UNAVAILABLE", "Controller sensor cannot be mapped to one physical node.")
    index, sensor = matches[0]
    node = _node_id(sensor.get("nodePhysicalId", sensor.get("targetNodePhysicalId")))
    direction = sensor.get("globalDirection")
    if direction is None:
        # The native linear export reports Sv and coordinate directions, not
        # sensor direction. Recover ONLY the actual configured physical row.
        try:
            row = linear["Sv"][index]
            dofs = linear["dofs"]
            if type(row) is not list or len(row) != len(dofs):
                raise ValueError()
            direction = [0.0, 0.0, 0.0]
            for weight, dof in zip(row, dofs):
                weight = finite(weight)
                if weight == 0:
                    continue
                if dof.get("unit") != "m" or _node_id(dof.get("nodePhysicalId")) != node:
                    raise ValueError()
                axis = dof["globalDirection"]
                if type(axis) is not list or len(axis) != 3:
                    raise ValueError()
                direction = [v + weight * finite(a) for v, a in zip(direction, axis)]
        except (KeyError, TypeError, ValueError, IndexError):
            fail("REFERENCE_UNAVAILABLE", "Controller sensor has no verified global physical axis.")
    if type(direction) is not list or len(direction) != 3:
        fail("REFERENCE_UNAVAILABLE", "Controller sensor has no global physical axis.")
    direction = [finite(v) for v in direction]
    nonzero = [i for i, v in enumerate(direction) if v != 0]
    if (len(nonzero) != 1 or abs(direction[nonzero[0]]) != 1 or sensor.get("unit") != "m/s"
            or sensor.get("quantity") != "Velocity"):
        fail("REFERENCE_UNAVAILABLE", "Reference requires an axis-aligned physical velocity sensor.")
    axis = "XYZ"[nonzero[0]]
    return f"node:{node}:velocity:{axis}", index, direction[nonzero[0]]


def _node_id(value):
    if type(value) is int and 1 <= value <= 2**32 - 1:
        return value
    if type(value) is str and re.fullmatch(r"[1-9][0-9]{0,9}", value) and int(value) <= 2**32 - 1:
        return int(value)
    fail("REFERENCE_UNAVAILABLE", "Physical source has no persistent node identity.")


def reference_identity(service, source):
    """Catalog integration helper; its input is a trusted registered native selector."""
    source = selector({**source, "kind": "native"})
    if source["mode"] != "active":
        fail("REFERENCE_UNAVAILABLE", "Independent reference belongs to the active native artifact.")
    pin = _catalog_pin(service, source)
    _, reference, linear, gains, _, reference_sha = _reference_material(service, source)
    channel, _, _ = _sensor_identity(linear, gains, reference)
    if _catalog_pin(service, source) != pin:
        fail("SOURCE_CHANGED", "Native source changed during reference lookup.")
    return {"sourceKind": "independent-reference", "referenceSha256": reference_sha,
            "selector": {"kind": "reference", "jobId": source["jobId"], "mode": "active",
                         "artifactId": source["artifactId"], "channelId": channel,
                         "referenceSha256": reference_sha}, "channelId": channel,
            "quantity": "velocity", "unit": "m/s", "modelSha256": pin["modelSha256"]}


def _reference(service, source, pin, identity, frames):
    job, reference, linear, gains, runtime, reference_sha = _reference_material(service, source)
    if source["referenceSha256"] != reference_sha:
        fail("SOURCE_CHANGED", "Independent reference identity changed.")
    channel, index, sign = _sensor_identity(linear, gains, reference)
    if channel != source["channelId"]:
        fail("REFERENCE_UNAVAILABLE", "Reference is available only at the configured physical velocity sensor.")
    if runtime is None:
        times = reference.get("timeOutputs", {}).get("timeSeconds")
        values = reference.get("timeOutputs", {}).get("fullVelocityMPerSecond")
    else:
        try:
            raw_trajectory = runtime["trajectory"]["v"]
            raw_row = linear["Sv"][index]
            if type(raw_trajectory) is not list or type(raw_row) is not list:
                raise ValueError()
            trajectory = np.asarray([[finite(v) for v in record] for record in raw_trajectory
                                     if type(record) is list], dtype=np.float64)
            if len(trajectory) != len(raw_trajectory):
                raise ValueError()
            row = np.asarray([finite(v) for v in raw_row], dtype=np.float64)
            if trajectory.ndim != 2 or row.shape != (trajectory.shape[1],) or not np.isfinite(trajectory).all() or not np.isfinite(row).all():
                raise ValueError()
            values = (trajectory @ row).tolist()
            dt = finite(job["result"]["configuration"]["dt"])
            times = [i * dt for i in range(len(values))]
        except (ValueError, TypeError, KeyError, IndexError):
            fail("REFERENCE_UNAVAILABLE", "Accepted reference lacks its physical velocity history.")
    if type(times) is not list or type(values) is not list or len(times) != len(values):
        fail("REFERENCE_UNAVAILABLE", "Independent reference history is incomplete.")
    times, values = [finite(t) for t in times], [finite(v) * sign for v in values]
    grid = [f["timeS"] for f in frames]
    if times != grid:
        # Only remove the actual independent initial condition, never trim a
        # partial native history or silently resample a different clock.
        if len(times) == len(grid) + 1 and times[0] == 0 and times[1:] == grid and grid[0] > 0:
            times, values = times[1:], values[1:]
        else:
            fail("INCOMPARABLE_RESULTS", "Independent reference and native history have different actual time grids.")
    samples = [{"time": t, "value": v, "recordNo": frame["recordNo"]}
               for t, v, frame in zip(times, values, frames)]
    if _reference_material(service, source)[-1] != reference_sha or _catalog_pin(service, source) != pin:
        fail("SOURCE_CHANGED", "Independent source changed during extraction.")
    return _dto(source, pin, identity, samples,
                {"proofScope": "independent-matrix-newmark-accepted-schedule/v1" if runtime is not None
                 else "independent-matrix-newmark-reference/v1", "referenceSha256": reference_sha,
                 "linearSha256": _bound_json(service, job, "linearPath")[1],
                 "gainsSha256": _bound_json(service, job, "gainsPath")[1],
                 "nativeGridSha256": digest(grid)})


def resolve_many(service, requested):
    sources = [selector(s) for s in requested]
    groups = {}
    resolved = {}
    for source in sources:
        if source["kind"] == "energy":
            resolved[canonical(source)] = energy_source(service, source)
            continue
        groups.setdefault(tuple(source[k] for k in ("jobId", "mode", "artifactId")), []).append(source)
    for group in groups.values():
        pin, identities, frames = _snapshot(service, group)
        ids = list(identities)
        proof_frames = [{k: f[k] for k in ("recordNo", "modelSha256", "originKind", "originSha256", "historyProofSha256")}
                        for f in frames]
        origin = {"proofScope": pin["proofScope"], "frameProvenanceSha256": digest(proof_frames),
                  "frameCount": len(frames)}
        if "nativeAcceptedIdentity" in pin:
            origin["nativeAcceptedCoverage"] = pin["nativeAcceptedCoverage"]
            origin["nativeAcceptedGrid"] = {k: pin["nativeAcceptedIdentity"][k] for k in ("dtS", "initialTimeS", "targetSteps")}
        for source in group:
            identity = identities[source["channelId"]]
            if source["kind"] == "reference":
                result = _reference(service, source, pin, identity, frames)
            else:
                column = ids.index(source["channelId"])
                samples = [{"time": f["timeS"], "value": f["values"][column], "recordNo": f["recordNo"]}
                           for f in frames]
                result = _dto(source, pin, identity, samples, origin)
                result["records"] = proof_frames
            resolved[canonical(source)] = result
    return [resolved[canonical(s)] for s in sources]


def energy_source(service, requested):
    """Derive mechanical energy from actual physical histories, checked matrices."""
    source = selector(requested)
    if source["kind"] != "energy":
        fail("CHANNEL_UNAVAILABLE", "Mechanical energy requires actual native physical histories.")
    job = _job(service, source["jobId"])
    linear, linear_sha = _bound_json(service, job, "linearPath")
    checked = job["result"].get("sourceReference")
    if (type(checked) is not dict or checked.get("matrixComparison") is None
            or "sourceReferencePath" not in job["result"]):
        fail("CHANNEL_UNAVAILABLE", "This source lacks independently checked frozen energy matrices.")
    checked_file, checked_sha = _bound_json(service, job, "sourceReferencePath")
    if checked_file != checked:
        fail("SOURCE_CHANGED", "Independent energy matrix evidence differs from its frozen artifact.")
    dofs = linear.get("dofs")
    if type(dofs) is not list or not 1 <= len(dofs) <= 8 or dofs != checked.get("dofs"):
        fail("CHANNEL_UNAVAILABLE", "Energy physical coordinate mapping is unavailable.")
    channels, signs = [], []
    for dof in dofs:
        direction = dof.get("globalDirection")
        if type(direction) is not list or len(direction) != 3:
            fail("CHANNEL_UNAVAILABLE", "Energy requires axis-aligned physical coordinates.")
        direction = [finite(v) for v in direction]
        nonzero = [i for i, v in enumerate(direction) if v != 0]
        if len(nonzero) != 1 or abs(direction[nonzero[0]]) != 1 or dof.get("unit") not in ("m", "rad"):
            fail("CHANNEL_UNAVAILABLE", "Energy requires physical displacement and velocity units.")
        node = _node_id(dof.get("nodePhysicalId"))
        quantities = ("displacement", "velocity") if dof["unit"] == "m" else ("rotation", "angularVelocity")
        channels.extend(f"node:{node}:{quantity}:{'XYZ'[nonzero[0]]}" for quantity in quantities)
        signs.append(direction[nonzero[0]])
    if len(set(channels)) != len(channels):
        fail("CHANNEL_UNAVAILABLE", "Energy coordinates must map to unique physical channels.")
    selectors = [{"kind": "native", **{k: source[k] for k in ("jobId", "mode", "artifactId")}, "channelId": c} for c in channels]
    pin, identities, frames = _snapshot(service, selectors)
    if checked.get("sourceSha256") != pin["modelSha256"]:
        fail("SOURCE_CHANGED", "Energy matrices belong to a different model source.")
    try:
        n = len(dofs)
        def matrix(raw):
            if type(raw) is not list or len(raw) != n or any(type(row) is not list or len(row) != n for row in raw):
                raise ValueError()
            return np.asarray([[finite(v) for v in row] for row in raw], dtype=np.float64)
        mass, stiffness = (matrix(checked[key]) for key in ("analyticalM", "analyticalK"))
        if any(a.shape != (n, n) or not np.isfinite(a).all() or not np.allclose(a, a.T, rtol=1e-12, atol=1e-14) for a in (mass, stiffness)):
            raise ValueError()
        for expected_matrix, key in ((mass, "M"), (stiffness, "K")):
            native = matrix(linear[key])
            if native.shape != expected_matrix.shape or not np.allclose(native, expected_matrix, rtol=1e-9, atol=max(1e-13, float(np.max(np.abs(expected_matrix))) * 1e-11)):
                raise ValueError()
        np.linalg.cholesky(mass)
        if float(np.min(np.linalg.eigvalsh(stiffness))) < -max(1e-13, float(np.max(np.abs(stiffness))) * 1e-11):
            raise ValueError()
        samples = []
        for f in frames:
            q = np.asarray(f["values"][0::2]) * signs
            v = np.asarray(f["values"][1::2]) * signs
            energy = finite(float(.5 * (q @ stiffness @ q + v @ mass @ v)))
            samples.append({"time": f["timeS"], "value": energy, "recordNo": f["recordNo"]})
    except (ValueError, KeyError, TypeError, np.linalg.LinAlgError):
        fail("CHANNEL_UNAVAILABLE", "Frozen independent energy matrices are unavailable or inconsistent.")
    if (_bound_json(service, job, "linearPath")[1] != linear_sha or
            _bound_json(service, job, "sourceReferencePath")[1] != checked_sha):
        fail("SOURCE_CHANGED", "Energy matrix artifact changed during extraction.")
    identity = {"channelId": "derived:mechanical-energy", "variableId": "derived-mechanical-energy",
                "quantity": "mechanicalEnergy", "axis": "scalar", "unit": "J",
                "nodeTarget": "all-mapped-physical-dofs"}
    result = _dto(source, pin, identity, samples,
                  {"method": "0.5*qT*K*q+0.5*vT*M*v", "linearSha256": linear_sha,
                   "proofScope": "derived-physical-dofs-independent-matrices/v1",
                   "independentMatrixEvidenceSha256": checked_sha,
                   "matrixSha256": digest({"M": mass.tolist(), "K": stiffness.tolist()}),
                   "frameProvenanceSha256": digest(frames)})
    result["metadata"].update(derived=True, derivation="0.5*qT*K*q+0.5*vT*M*v",
                              physicalChannelIds=channels, sourceKind="derived-mechanical-energy",
                              underlyingSourceKind=pin["sourceKind"], linearSha256=linear_sha,
                              matrixSha256=result["dto"]["origin"]["matrixSha256"])
    return result
