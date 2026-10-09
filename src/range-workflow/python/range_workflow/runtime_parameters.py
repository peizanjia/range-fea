"""Portable host validation of the native accepted runtime parameter codec.

Hashes follow the native binary v1 specification; JSON byte formatting is not
used as a cross-language parameter identity. This module grants no approval.
"""
from __future__ import annotations

import copy
import hashlib
import math
import re
import struct
import uuid

SCHEMA = "range.structural-runtime-parameters/v1"
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_\-.:/]{0,127}\Z")
_DECIMAL = re.compile(r"(?:0|[1-9][0-9]{0,19})\Z")
_FIELDS = {"revision", "velocityGainNsm", "actuatorLimits", "fixedBaseControllerRevision",
           "parameterSha256", "runtimeControllerRevision"}


def _exact(value, fields):
    if type(value) is not dict or set(value) != fields:
        raise ValueError("Invalid runtime parameter object shape")


def decimal(value):
    if type(value) is not str or not _DECIMAL.fullmatch(value) or int(value) > 2**64 - 1:
        raise ValueError("Runtime sequence or revision must be canonical unsigned decimal")
    return int(value)


def _number(value):
    if type(value) not in (int, float):
        raise ValueError("Runtime parameter must be a finite number")
    try:
        result = float(value)
    except (ValueError, OverflowError):
        raise ValueError("Runtime parameter must be finite") from None
    if not math.isfinite(result):
        raise ValueError("Runtime parameter must be finite")
    return 0.0 if result == 0 else result


def limits(value):
    if type(value) is not list or not 1 <= len(value) <= 256:
        raise ValueError("Runtime actuator count must be 1..256")
    result = []
    for raw in value:
        _exact(raw, {"actuatorId", "minN", "maxN", "rateNPerS"})
        actuator_id = raw["actuatorId"]
        if type(actuator_id) is not str or not _ID.fullmatch(actuator_id):
            raise ValueError("Invalid runtime actuator identity")
        lo, hi, rate = (_number(raw[key]) for key in ("minN", "maxN", "rateNPerS"))
        if lo > hi or rate < 0:
            raise ValueError("Invalid runtime actuator limits")
        result.append(dict(actuatorId=actuator_id, minN=lo, maxN=hi, rateNPerS=rate))
    result.sort(key=lambda item: item["actuatorId"])
    if len({item["actuatorId"] for item in result}) != len(result):
        raise ValueError("Duplicate runtime actuator")
    return result


def make_parameters(base_revision, revision, gain, actuator_limits):
    if type(base_revision) is not str or not _SHA.fullmatch(base_revision):
        raise ValueError("Invalid fixed base controller digest")
    if type(revision) is not int or not 0 <= revision <= 2**64 - 1:
        raise ValueError("Invalid parameter revision")
    gain = _number(gain)
    if not 1e-9 <= gain <= 1e12:
        raise ValueError("Velocity gain must be in [1e-9, 1e12] N*s/m")
    normalized = limits(actuator_limits)
    encoded = bytearray(b"range.structural-runtime-parameters/hash/v1\x00")
    encoded.extend(bytes.fromhex(base_revision))
    encoded.extend(struct.pack(">QdI", revision, gain, len(normalized)))
    for item in normalized:
        raw_id = item["actuatorId"].encode("utf-8")
        encoded.extend(struct.pack(">I", len(raw_id)))
        encoded.extend(raw_id)
        encoded.extend(struct.pack(">ddd", item["minN"], item["maxN"], item["rateNPerS"]))
    parameter_sha = hashlib.sha256(encoded).hexdigest()
    runtime_bytes = b"".join(struct.pack(">I", len(text)) + text.encode("ascii")
                             for text in (base_revision, parameter_sha))
    return {"revision": str(revision), "velocityGainNsm": gain, "actuatorLimits": normalized,
            "fixedBaseControllerRevision": base_revision, "parameterSha256": parameter_sha,
            "runtimeControllerRevision": hashlib.sha256(runtime_bytes).hexdigest()}


def validate_parameters(value):
    _exact(value, _FIELDS)
    expected = make_parameters(value["fixedBaseControllerRevision"], decimal(value["revision"]),
                               value["velocityGainNsm"], value["actuatorLimits"])
    if value != expected:
        raise ValueError("Runtime parameters have invalid hashes or noncanonical ordering")
    return expected


def validate_state(value):
    _exact(value, {"schema", "parameters", "history"})
    if value["schema"] != SCHEMA:
        raise ValueError("Unsupported runtime state schema")
    parameters = validate_parameters(value["parameters"])
    history = value["history"]
    if type(history) is not list or len(history) > 128 or len(history) != decimal(parameters["revision"]):
        raise ValueError("Invalid accepted parameter history length")
    seen, previous_seq, previous_time = set(), 0, -math.inf
    for revision, change in enumerate(history, 1):
        _exact(change, {"proposalId", "previousRevision", "revision", "appliedSeq",
                        "acceptedTimeS", "parameterSha256"})
        try:
            proposal = str(uuid.UUID(change["proposalId"]))
        except (ValueError, TypeError, AttributeError):
            raise ValueError("Invalid accepted proposal identity") from None
        if proposal != change["proposalId"] or uuid.UUID(proposal).int == 0 or proposal in seen:
            raise ValueError("Invalid or repeated accepted proposal identity")
        seq, time = decimal(change["appliedSeq"]), _number(change["acceptedTimeS"])
        if (decimal(change["revision"]) != revision or decimal(change["previousRevision"]) != revision - 1
                or seq <= previous_seq or time < 0 or time <= previous_time
                or type(change["parameterSha256"]) is not str or not _SHA.fullmatch(change["parameterSha256"])):
            raise ValueError("Invalid accepted runtime history")
        seen.add(proposal)
        previous_seq, previous_time = seq, time
    if history and history[-1]["parameterSha256"] != parameters["parameterSha256"]:
        raise ValueError("Current parameter digest disagrees with accepted history")
    return copy.deepcopy(value)


def candidate(accepted_state, gain, actuator_limits, immutable_limits):
    accepted = validate_state(accepted_state)
    current = accepted["parameters"]
    proposed = make_parameters(current["fixedBaseControllerRevision"],
                               decimal(current["revision"]) + 1, gain, actuator_limits)
    base = limits(immutable_limits)
    if len(base) != len(proposed["actuatorLimits"]):
        raise ValueError("Runtime actuator catalog changed")
    for original, changed in zip(base, proposed["actuatorLimits"]):
        if (original["actuatorId"] != changed["actuatorId"] or changed["minN"] < original["minN"]
                or changed["maxN"] > original["maxN"]
                or (original["rateNPerS"] > 0 and
                    (changed["rateNPerS"] == 0 or changed["rateNPerS"] > original["rateNPerS"]))):
            raise ValueError("Runtime limits exceed immutable model envelope")
    if len(accepted["history"]) >= 128:
        raise ValueError("Accepted runtime history is full")
    return proposed
