"""Pure, bounded analysis of complete workflow time histories.

This module accepts already-resolved data transfer objects.  It has no RPC,
filesystem, or code-execution boundary; callers must resolve and authenticate
sources before passing them here.
"""
from __future__ import annotations

import copy
import math
import re
from typing import Any

import numpy as np


METHOD_VERSION = "range.series-analysis/v1"
MAX_SOURCE_SAMPLES = 10_000
MAX_ORIGIN_NODES = 200_000
MAX_ORIGIN_TEXT_BYTES = 4 * 1024 * 1024
MAX_ORIGIN_DEPTH = 8
MAX_ORIGIN_STRING_CHARS = 4096
_HASH = re.compile(r"^[0-9a-f]{64}$")
_IDENTIFIER = re.compile(r"^[A-Za-z0-9._:/-]{1,256}$")
_LABEL = re.compile(r"^[A-Za-z0-9][A-Za-z0-9./*^_-]{0,63}$")
_SOURCE_FIELDS = {
    "sourceKind", "jobId", "artifactId", "modelSha256", "channelId",
    "axis", "variable", "unit", "timeUnit", "completed", "sourceHash",
    "samples",
}
_OPTIONAL_SOURCE_FIELDS = {"origin"}
_SAMPLE_FIELDS = {"time", "value", "recordNo"}
_OPTIONAL_SAMPLE_FIELDS = {"origin"}
_SOURCE_KINDS = {
    "native-active-fe", "native-passive-fe", "independent-reference", "caller-series",
}


class SeriesAnalysisError(ValueError):
    """Raised when a source or requested analysis violates the frozen contract."""


def _fail(message: str) -> None:
    raise SeriesAnalysisError(message)


def _bounded_text(value: Any, field: str, pattern: re.Pattern[str]) -> str:
    if type(value) is not str or not pattern.fullmatch(value):
        _fail(f"{field} must be a bounded identifier")
    return value


def _number(value: Any, field: str) -> float:
    if type(value) not in (int, float):
        _fail(f"{field} must be a finite number")
    try:
        number = float(value)
    except (OverflowError, ValueError):
        _fail(f"{field} must be a finite number")
    # The bound matches the workflow's other numeric DTOs and prevents an
    # otherwise finite JSON number from overflowing intermediate FFT products.
    if not math.isfinite(number) or abs(number) > 1.0e100:
        _fail(f"{field} is outside the supported finite range")
    return number


def _utf8_size(value: str, field: str) -> int:
    try:
        return len(value.encode("utf-8", "strict"))
    except UnicodeEncodeError:
        _fail(f"{field} contains invalid Unicode text")


def _validate_metadata(value: Any, field: str, budget: dict[str, int], *, depth: int = 0) -> None:
    """Preflight bounded JSON origin data, sharing limits across the DTO."""
    budget["nodes"] -= 1
    if budget["nodes"] < 0 or depth > MAX_ORIGIN_DEPTH:
        _fail(f"{field} exceeds the aggregate metadata limit")
    if value is None or type(value) is bool:
        return
    if type(value) is str:
        if len(value) > MAX_ORIGIN_STRING_CHARS:
            _fail(f"{field} contains an oversized string")
        budget["textBytes"] -= _utf8_size(value, field)
        if budget["textBytes"] < 0:
            _fail("source origins exceed the aggregate 4 MiB text limit")
        return
    if type(value) in (int, float):
        _number(value, field)
        return
    if type(value) is list:
        for item in value:
            _validate_metadata(item, field, budget, depth=depth + 1)
        return
    if type(value) is dict:
        if len(value) > 128 or any(type(key) is not str or len(key) > 128 for key in value):
            _fail(f"{field} contains invalid metadata keys")
        for key, item in value.items():
            budget["textBytes"] -= _utf8_size(key, field)
            if budget["textBytes"] < 0:
                _fail("source origins exceed the aggregate 4 MiB text limit")
            _validate_metadata(item, field, budget, depth=depth + 1)
        return
    _fail(f"{field} must contain JSON-compatible metadata")


def _copy_metadata(value: Any) -> Any:
    """Copy metadata after a whole-DTO preflight has succeeded."""
    if type(value) is dict:
        return {key: _copy_metadata(item) for key, item in value.items()}
    if type(value) is list:
        return [_copy_metadata(item) for item in value]
    return value


def validate_series(value: Any) -> dict:
    """Validate and normalize one strict v1 source-history DTO.

    Required fields identify the source and physical channel.  ``origin`` is
    optional metadata and is copied as inert JSON data.  Every sample carries
    its actual time in seconds, value, and original record number.
    """
    if type(value) is not dict:
        _fail("source must be an object")
    keys = set(value)
    if not _SOURCE_FIELDS <= keys or keys - _SOURCE_FIELDS - _OPTIONAL_SOURCE_FIELDS:
        _fail("source has missing or unsupported fields")
    source_kind = _bounded_text(value["sourceKind"], "sourceKind", _IDENTIFIER)
    if source_kind not in _SOURCE_KINDS:
        _fail("sourceKind is not a supported, explicit source label")
    job_id = _bounded_text(value["jobId"], "jobId", _IDENTIFIER)
    artifact_id = _bounded_text(value["artifactId"], "artifactId", _IDENTIFIER)
    model_sha = value["modelSha256"]
    source_hash = value["sourceHash"]
    if (type(model_sha) is not str or not _HASH.fullmatch(model_sha) or
            type(source_hash) is not str or not _HASH.fullmatch(source_hash)):
        _fail("modelSha256 and sourceHash must be lowercase SHA-256 digests")
    channel_id = _bounded_text(value["channelId"], "channelId", _IDENTIFIER)
    axis = _bounded_text(value["axis"], "axis", _LABEL)
    variable = _bounded_text(value["variable"], "variable", _IDENTIFIER)
    unit = _bounded_text(value["unit"], "unit", _LABEL)
    if type(value["timeUnit"]) is not str or value["timeUnit"] != "s":
        _fail("timeUnit must be s")
    if type(value["completed"]) is not bool:
        _fail("completed must be a boolean")

    raw_samples = value["samples"]
    minimum_count = 4 if value["completed"] else 1
    if (type(raw_samples) is not list or
            not minimum_count <= len(raw_samples) <= MAX_SOURCE_SAMPLES):
        _fail(f"samples must contain {minimum_count}..{MAX_SOURCE_SAMPLES} actual records")
    samples = []
    metadata_references = []
    if "origin" in value:
        metadata_references.append(("origin", value["origin"], None))
    previous_time = -math.inf
    previous_record = 0
    for index, sample in enumerate(raw_samples):
        if type(sample) is not dict:
            _fail(f"samples[{index}] must be an object")
        sample_keys = set(sample)
        if (not _SAMPLE_FIELDS <= sample_keys or
                sample_keys - _SAMPLE_FIELDS - _OPTIONAL_SAMPLE_FIELDS):
            _fail(f"samples[{index}] has missing or unsupported fields")
        time = _number(sample["time"], f"samples[{index}].time")
        sample_value = _number(sample["value"], f"samples[{index}].value")
        record_no = sample["recordNo"]
        if (type(record_no) is not int or record_no <= previous_record or
                record_no > (2 ** 63 - 1)):
            _fail("recordNo values must be strictly increasing positive integers")
        if time <= previous_time:
            _fail("sample times must be strictly increasing")
        normalized = {"time": time, "value": sample_value, "recordNo": record_no}
        if "origin" in sample:
            metadata_references.append((f"samples[{index}].origin", sample["origin"], index))
        samples.append(normalized)
        previous_time, previous_record = time, record_no

    metadata_budget = {"nodes": MAX_ORIGIN_NODES, "textBytes": MAX_ORIGIN_TEXT_BYTES}
    for field, metadata, _sample_index in metadata_references:
        _validate_metadata(metadata, field, metadata_budget)
    normalized_source_origin = None
    for _field, metadata, sample_index in metadata_references:
        normalized_origin = _copy_metadata(metadata)
        if sample_index is None:
            normalized_source_origin = normalized_origin
        else:
            samples[sample_index]["origin"] = normalized_origin

    result = {
        "sourceKind": source_kind,
        "jobId": job_id,
        "artifactId": artifact_id,
        "modelSha256": model_sha,
        "channelId": channel_id,
        "axis": axis,
        "variable": variable,
        "unit": unit,
        "timeUnit": "s",
        "completed": value["completed"],
        "sourceHash": source_hash,
        "samples": samples,
    }
    if "origin" in value:
        result["origin"] = normalized_source_origin
    return result


def _provenance(series: dict) -> dict:
    return {key: copy.deepcopy(value) for key, value in series.items() if key != "samples"}


def _result(operation: str, provenance: Any, units: dict, **fields: Any) -> dict:
    return {"methodVersion": METHOD_VERSION, "operation": operation,
            "provenance": provenance, "units": units, **fields}


def _uniform_interval(series: dict) -> float:
    times = [sample["time"] for sample in series["samples"]]
    dt = times[1] - times[0]
    if not math.isfinite(dt) or dt <= 0:
        _fail("sample interval must be finite and positive")
    tolerance = max(1.0e-12, 1.0e-9 * dt)
    if any(abs((right - left) - dt) > tolerance for left, right in zip(times, times[1:])):
        _fail("frequency analysis requires a uniform time grid; interpolation is not performed")
    return dt


def _require_complete(series: dict) -> None:
    if not series["completed"]:
        _fail("this analysis requires a completed history")
    if len(series["samples"]) < 4:
        _fail("complete analysis requires at least four actual records")


def spectrum(source: Any, *, window: str = "hann", remove_mean: bool = True) -> dict:
    """Return single-sided amplitude and power spectral density arrays.

    Uses a periodic Hann window by default.  Amplitude is normalized by
    coherent gain (sum(window)); PSD is normalized by window power and sample
    rate.  DC and the even-length Nyquist bin are never doubled.
    """
    series = validate_series(source)
    _require_complete(series)
    if type(window) is not str or window not in ("hann", "rectangular"):
        _fail("window must be hann or rectangular")
    if type(remove_mean) is not bool:
        _fail("remove_mean must be a boolean")
    dt = _uniform_interval(series)
    values = np.asarray([sample["value"] for sample in series["samples"]], dtype=np.float64)
    if remove_mean:
        # fsum avoids an unnecessary loss of precision when values have a
        # large common offset and a comparatively small oscillatory part.
        mean = math.fsum(float(item) for item in values) / len(values)
        values = values - mean
    count = len(values)
    weights = (0.5 - 0.5 * np.cos(2.0 * math.pi * np.arange(count) / count)
               if window == "hann" else np.ones(count, dtype=np.float64))
    coherent_gain = float(np.sum(weights))
    window_power = float(np.dot(weights, weights))
    try:
        sample_rate = 1.0 / dt
    except OverflowError:
        _fail("sampling rate cannot be represented as a finite number")
    if (not math.isfinite(sample_rate) or sample_rate <= 0.0 or
            not math.isfinite(coherent_gain) or coherent_gain <= 0.0 or
            not math.isfinite(window_power) or window_power <= 0.0):
        _fail("sampling rate, sample interval, or window cannot be represented safely")

    transformed = np.fft.rfft(values * weights)
    magnitude = np.abs(transformed)
    amplitude = magnitude / coherent_gain
    # Scale the magnitude before squaring.  Forming |FFT|^2 first can overflow
    # at large input magnitudes or underflow for small signals; multiplying
    # fs by window_power can also overflow even when the PSD itself is finite.
    psd_root = magnitude / math.sqrt(window_power) / math.sqrt(sample_rate)
    psd = psd_root * psd_root
    one_sided_end = len(amplitude) - (1 if count % 2 == 0 else 0)
    if one_sided_end > 1:
        amplitude[1:one_sided_end] *= 2.0
        psd[1:one_sided_end] *= 2.0
    frequencies = np.fft.rfftfreq(count, d=dt)
    if np.any((magnitude > 0.0) & (psd == 0.0)):
        _fail("PSD underflows the supported floating-point range")
    if (not np.isfinite(frequencies).all() or not np.isfinite(amplitude).all() or
            not np.isfinite(psd).all()):
        _fail("spectrum output exceeds the finite supported range")

    return _result(
        "spectrum", {"source": _provenance(series)},
        {"frequency": "Hz", "amplitude": series["unit"],
         "psd": {"numeratorUnit": series["unit"], "power": 2,
                 "denominatorUnit": "Hz"}},
        sampleCount=count, sampleIntervalS=dt, samplingRateHz=sample_rate,
        window=window, removeMean=remove_mean,
        frequenciesHz=frequencies.tolist(), amplitude=amplitude.tolist(), psd=psd.tolist(),
    )


def _require_same_grid(left: dict, right: dict) -> None:
    if len(left["samples"]) != len(right["samples"]):
        _fail("histories do not have the same actual time grid")
    for a, b in zip(left["samples"], right["samples"]):
        if a["time"] != b["time"]:
            _fail("histories do not have the same actual time grid")


def _require_same_model(left: dict, right: dict) -> None:
    if left["modelSha256"] != right["modelSha256"]:
        _fail("histories come from different model snapshots")


def compare_series(left_source: Any, right_source: Any) -> dict:
    """Return left-minus-right error for one identical physical channel/grid."""
    left, right = validate_series(left_source), validate_series(right_source)
    _require_complete(left)
    _require_complete(right)
    _require_same_model(left, right)
    for field in ("channelId", "axis", "variable", "unit", "timeUnit"):
        if left[field] != right[field]:
            _fail("histories differ in physical channel, axis, variable, or unit")
    _require_same_grid(left, right)
    samples = []
    for a, b in zip(left["samples"], right["samples"]):
        difference = a["value"] - b["value"]
        if not math.isfinite(difference):
            _fail("comparison output is outside the finite supported range")
        samples.append({"time": a["time"], "leftRecordNo": a["recordNo"],
                        "rightRecordNo": b["recordNo"], "value": difference})
    return _result("error-series", {"left": _provenance(left), "right": _provenance(right)},
                   {"time": "s", "value": left["unit"]}, samples=samples)


def phase_plot(x_source: Any, y_source: Any) -> dict:
    """Pair two explicitly unit-bearing channels on one model and time grid."""
    x_series, y_series = validate_series(x_source), validate_series(y_source)
    _require_complete(x_series)
    _require_complete(y_series)
    _require_same_model(x_series, y_series)
    _require_same_grid(x_series, y_series)
    samples = [{"time": x["time"], "xRecordNo": x["recordNo"],
                "yRecordNo": y["recordNo"],
                "x": x["value"], "y": y["value"]}
               for x, y in zip(x_series["samples"], y_series["samples"])]
    return _result("phase-plot", {"x": _provenance(x_series), "y": _provenance(y_series)},
                   {"time": "s", "x": x_series["unit"], "y": y_series["unit"]},
                   samples=samples)


def envelope_samples(source: Any, *, limit: int) -> dict:
    """Return bounded display points using first/min/max/last per bucket.

    The source history remains untouched.  Original order and record identity
    are preserved, duplicate selections collapse by record index, and both
    global endpoints are always kept.
    """
    series = validate_series(source)
    if type(limit) is not int or limit < 8:
        _fail("display limit must be an integer of at least 8")
    samples = series["samples"]
    if len(samples) <= limit:
        selected = samples
    else:
        bucket_count = max(1, (limit - 2) // 4)
        interior = samples[1:-1]
        selected_indices = {0, len(samples) - 1}
        interior_count = len(interior)
        for bucket in range(bucket_count):
            start = (bucket * interior_count) // bucket_count
            end = ((bucket + 1) * interior_count) // bucket_count
            if start >= end:
                continue
            indices = range(start + 1, end + 1)
            first, last = start + 1, end
            minimum = min(indices, key=lambda index: samples[index]["value"])
            maximum = max(indices, key=lambda index: samples[index]["value"])
            selected_indices.update((first, minimum, maximum, last))
        selected = [samples[index] for index in sorted(selected_indices)]
    if len(selected) > limit:
        # This is an invariant of the first/min/max/last bucket allocation.
        # Keep a clear error instead of silently dropping peaks if changed.
        _fail("display envelope exceeded its requested output limit")
    return _result("display-envelope", _provenance(series),
                   {"time": "s", "value": series["unit"]},
                   sourceSampleCount=len(samples), limit=limit,
                   coverageComplete=series["completed"], displayOnly=True,
                   samples=copy.deepcopy(selected))
