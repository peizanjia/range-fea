"""Read-only plot/raw workbench over authenticated histories.

Display reduction is never used by FFT or raw export. Exports live in an owned
derived directory and are retrieved through bounded, hash-checked chunks.
"""
from __future__ import annotations

import base64
import copy
import csv
import hashlib
import io
import os
from pathlib import Path
import threading
import uuid

from . import series_sources as sources
from .series_analysis import METHOD_VERSION, SeriesAnalysisError, compare_series, phase_plot, spectrum
from .service import WorkflowError, canonical, exact, file_hash, identifier, integer

_VIEWS = {"time", "error", "phase", "amplitude", "psd", "energy", "work"}
_MAX_EXPORT_BYTES = 32 * 1024 * 1024
_EXPORT_LOCK = threading.RLock()


def _source_list(value):
    if type(value) is not list or not 1 <= len(value) <= 4:
        sources.fail("INVALID_PARAMETERS", "Select one through four authenticated sources.")
    return [sources.selector(s) for s in value]


def _options(value):
    exact(value, set(), {"limit", "window", "removeMean"})
    limit = integer(value.get("limit", 1000), "limit", 8, 1000)
    window = value.get("window", "hann")
    remove_mean = value.get("removeMean", True)
    if type(window) is not str or window not in ("hann", "rectangular") or type(remove_mean) is not bool:
        sources.fail("INVALID_PARAMETERS", "Window or mean removal option is invalid.")
    return limit, window, remove_mean


def _display(points, limit):
    """Ordered first/min/max/last buckets with original point/bin ordinals."""
    if len(points) <= limit:
        return copy.deepcopy(points)
    indices = {0, len(points) - 1}
    interior = len(points) - 2
    count = max(1, (limit - 2) // 4)
    for bucket in range(count):
        start = 1 + bucket * interior // count
        end = 1 + (bucket + 1) * interior // count
        if start >= end:
            continue
        interval = range(start, end)
        indices.update((start, end - 1, min(interval, key=lambda i: points[i]["y"]),
                        max(interval, key=lambda i: points[i]["y"])))
    return [copy.deepcopy(points[i]) for i in sorted(indices)]


def _trace(index, label, unit, metadata, points, raw_count, limit):
    display = _display(points, limit)
    return {"id": f"trace-{index + 1}", "label": label, "unit": unit,
            "sources": copy.deepcopy(metadata), "points": display,
            "rawSampleCount": raw_count, "displaySampleCount": len(display), "displayOnly": True}


def _label(resolved):
    metadata = resolved["metadata"]
    chosen = metadata["selector"]
    mode = ("reference" if metadata["sourceKind"] == "independent-reference" else
            "derived energy" if metadata["sourceKind"] == "derived-mechanical-energy" else chosen["mode"])
    return f"{chosen['jobId'][:8]} / {mode} / {chosen.get('channelId', 'mechanical-energy')}"


def _time_points(dto):
    return [{"x": s["time"], "y": s["value"], "ordinal": i}
            for i, s in enumerate(dto["samples"])]


def _plot(service, parameters):
    exact(parameters, {"view", "sources"}, {"options"})
    view = parameters["view"]
    if type(view) is not str or view not in _VIEWS:
        sources.fail("INVALID_PARAMETERS", "Plot view is unsupported.")
    selected = _source_list(parameters["sources"])
    limit, window, remove_mean = _options(parameters.get("options", {}))
    if view in ("error", "phase") and len(selected) != 2:
        sources.fail("INVALID_PARAMETERS", "Error and phase views require exactly two sources.")
    if view == "work":
        traces, unavailable, export_sources = [], [], []
        for index, chosen in enumerate(selected):
            try:
                if chosen["kind"] != "native" or chosen["mode"] != "active":
                    sources.fail("CHANNEL_UNAVAILABLE", "Accepted controller work is available only on actual active native records")
                if chosen["channelId"] not in {"control:work", "piezo:electricalWork"}:
                    held = sources.resolve_many(service, [chosen])[0]
                    work_channel = "piezo:electricalWork" if "nativeAcceptedIdentity" in held["pin"] else "control:work"
                    chosen = {**{k: chosen[k] for k in ("kind", "jobId", "mode", "artifactId")}, "channelId": work_channel}
                actual = sources.resolve_many(service, [chosen])[0]
                traces.append(_trace(index, _label(actual), "J", [actual["metadata"]],
                    _time_points(actual["dto"]), len(actual["dto"]["samples"]), limit))
                export_sources.append(actual["metadata"]["selector"])
            except WorkflowError as error:
                if error.code not in {"CHANNEL_UNAVAILABLE", "UNSUPPORTED_SOURCE"}:
                    raise
                unavailable.append({"source": chosen, "code": error.code, "message": error.message})
        return {"schemaId": "range.series-plot/v1", "methodVersion": METHOD_VERSION,
            "view": view, "title": "Accepted cumulative work",
            "axes": {"x": {"label": "Time", "unit": "s"}, "y": {"label": "Cumulative work", "unit": "J"}},
            "traces": traces, "unavailable": unavailable,
            "export": {"available": bool(export_sources), "scope": "complete-raw", "sources": export_sources}}
    if view == "energy":
        traces, unavailable, export_sources = [], [], []
        for index, chosen in enumerate(selected):
            try:
                if chosen["kind"] == "native":
                    # A held native selector is checked before changing to the
                    # fixed mechanical-energy selector with its own identity.
                    sources.resolve_many(service, [chosen])
                    chosen = {"kind": "energy", **{k: chosen[k] for k in ("jobId", "mode", "artifactId")}}
                derived = sources.energy_source(service, chosen)
                traces.append(_trace(index, _label(derived), "J", [derived["metadata"]],
                                     _time_points(derived["dto"]), len(derived["dto"]["samples"]), limit))
                export_sources.append(derived["metadata"]["selector"])
            except WorkflowError as error:
                if error.code not in ("CHANNEL_UNAVAILABLE", "REFERENCE_UNAVAILABLE"):
                    raise
                unavailable.append({"source": chosen, "code": error.code, "message": error.message})
        return {"schemaId": "range.series-plot/v1", "methodVersion": METHOD_VERSION,
                "view": view, "title": "Derived mechanical energy",
                "axes": {"x": {"label": "Time", "unit": "s"},
                         "y": {"label": "Derived mechanical energy", "unit": "J"}},
                "traces": traces, "unavailable": unavailable,
                "export": {"available": bool(export_sources), "scope": "complete-raw", "sources": export_sources}}
    resolved = sources.resolve_many(service, selected)
    metadata = [r["metadata"] for r in resolved]
    dtos = [r["dto"] for r in resolved]
    export = {"available": True, "scope": "complete-raw", "sources": [m["selector"] for m in metadata]}
    unavailable, traces = [], []
    axes = {"x": {"label": "Time", "unit": "s"},
            "y": {"label": view.title(), "unit": dtos[0]["unit"]}}
    try:
        if view == "time":
            if len({d["unit"] for d in dtos}) != 1:
                sources.fail("INCOMPARABLE_RESULTS", "Time overlays require identical physical units.")
            quantities = {m["channel"]["quantity"] for m in metadata}
            axes["y"]["label"] = next(iter(quantities)).replace("mechanicalEnergy", "Mechanical energy").title() if len(quantities) == 1 else "Value"
            for i, r in enumerate(resolved):
                traces.append(_trace(i, _label(r), r["dto"]["unit"], [r["metadata"]],
                                     _time_points(r["dto"]), len(r["dto"]["samples"]), limit))
        elif view == "error":
            # Include physical node identity, even if two DTO labels look alike.
            if metadata[0]["channel"] != metadata[1]["channel"]:
                sources.fail("INCOMPARABLE_RESULTS", "Error requires the same physical channel and unit.")
            analysis = compare_series(*dtos)
            points = [{"x": s["time"], "y": s["value"], "ordinal": i}
                      for i, s in enumerate(analysis["samples"])]
            traces.append(_trace(0, "Left minus right", dtos[0]["unit"], metadata, points, len(points), limit))
        elif view == "phase":
            analysis = phase_plot(*dtos)
            axes = {"x": {"label": _label(resolved[0]), "unit": dtos[0]["unit"]},
                    "y": {"label": _label(resolved[1]), "unit": dtos[1]["unit"]}}
            points = [{"x": s["x"], "y": s["y"], "ordinal": i}
                      for i, s in enumerate(analysis["samples"])]
            traces.append(_trace(0, "Phase", dtos[1]["unit"], metadata, points, len(points), limit))
        elif view in ("amplitude", "psd"):
            if len({d["unit"] for d in dtos}) != 1:
                sources.fail("INCOMPARABLE_RESULTS", "Spectral overlays require identical physical units.")
            unit = dtos[0]["unit"] if view == "amplitude" else f"({dtos[0]['unit']})^2/Hz"
            axes = {"x": {"label": "Frequency", "unit": "Hz"},
                    "y": {"label": "Amplitude" if view == "amplitude" else "Power spectral density", "unit": unit}}
            for i, r in enumerate(resolved):
                analysis = spectrum(r["dto"], window=window, remove_mean=remove_mean)
                points = [{"x": frequency, "y": value, "ordinal": j}
                          for j, (frequency, value) in enumerate(zip(analysis["frequenciesHz"], analysis[view]))]
                trace = _trace(i, _label(r), unit, [r["metadata"]], points,
                               len(r["dto"]["samples"]), limit)
                trace.update(spectralBinCount=len(points), window=window, removeMean=remove_mean)
                traces.append(trace)
        elif view == "work":
            axes["y"] = {"label": "Persisted control work", "unit": "J"}
            unavailable = [{"source": m["selector"], "code": "CHANNEL_UNAVAILABLE",
                            "message": "No actual persisted control-work channel is registered."} for m in metadata]
            export = {"available": False, "scope": "complete-raw", "sources": []}
    except SeriesAnalysisError as error:
        sources.fail("INCOMPARABLE_RESULTS", str(error))
    result = {"schemaId": "range.series-plot/v1", "methodVersion": METHOD_VERSION,
              "view": view, "title": view.title(), "axes": axes, "traces": traces,
              "unavailable": unavailable, "export": export}
    if len(canonical(result)) > 2 * 1024 * 1024:
        sources.fail("RESULT_TOO_LARGE", "Bounded plot response exceeds its byte budget.")
    return result


def _raw(service, parameters):
    exact(parameters, {"source"}, {"offset", "limit"})
    chosen = sources.selector(parameters["source"])
    offset = integer(parameters.get("offset", 0), "offset", 0, sources.MAX_SAMPLES)
    limit = integer(parameters.get("limit", 200), "limit", 1, 200)
    resolved = sources.resolve_many(service, [chosen])[0]
    samples = resolved["dto"]["samples"]
    if offset > len(samples):
        sources.fail("INVALID_PARAMETERS", "Raw offset exceeds the complete source record count.")
    points = [{"timeS": s["time"], "value": s["value"], "recordNo": s["recordNo"], "ordinal": i}
              for i, s in enumerate(samples[offset:offset + limit], offset)]
    if "records" in resolved:
        for point, record in zip(points, resolved["records"][offset:offset + limit]):
            point.update(copy.deepcopy(record))
    return {"schemaId": "range.series-raw/v1", "methodVersion": METHOD_VERSION,
            "source": resolved["metadata"], "sourceHash": resolved["dto"]["sourceHash"],
            "rawSampleCount": len(samples), "offset": offset, "limit": limit,
            "points": points, "nextOffset": offset + len(points) if offset + len(points) < len(samples) else None,
            "origin": resolved["dto"]["origin"], "displayOnly": False}


def _export_root(service, create=False):
    root = Path(service.workspace) / "derived" / "series-exports"
    try:
        workspace = Path(service.workspace)
        if workspace.is_symlink():
            raise ValueError()
        cursor = workspace
        for component in ("derived", "series-exports"):
            cursor /= component
            if cursor.is_symlink():
                raise ValueError()
            if create:
                cursor.mkdir(exist_ok=True)
            if not cursor.is_dir():
                raise ValueError()
        root.resolve(strict=True).relative_to(workspace.resolve(strict=True))
    except (OSError, ValueError, RuntimeError):
        sources.fail("ARTIFACT_PATH", "Managed export directory is unavailable or unsafe.")
    return root


def _exports(service):
    if not hasattr(service, "_series_exports"):
        service._series_exports = {}
    return service._series_exports


def _export(service, parameters):
    exact(parameters, {"sources", "range"})
    if parameters["range"] != "complete":
        sources.fail("INVALID_PARAMETERS", "Raw exports require the complete source range.")
    selected = _source_list(parameters["sources"])
    if any("sourceHash" not in s for s in selected):
        sources.fail("INVALID_PARAMETERS", "Export requires source hashes from a held plot or raw query.")
    resolved = sources.resolve_many(service, selected)
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(("source", "sourceKind", "jobId", "mode", "artifactId", "channelId", "unit", "ordinal", "recordNo", "timeS", "value", "sourceHash"))
    count = 0
    for index, result in enumerate(resolved):
        metadata = result["metadata"]
        chosen = metadata["selector"]
        for ordinal, point in enumerate(result["dto"]["samples"]):
            writer.writerow((f"source-{index + 1}", metadata["sourceKind"], chosen["jobId"], chosen["mode"],
                             chosen["artifactId"], chosen.get("channelId", "derived:mechanical-energy"), result["dto"]["unit"], ordinal,
                             point["recordNo"], repr(point["time"]), repr(point["value"]), metadata["sourceHash"]))
            count += 1
    encoded = output.getvalue().encode("utf-8")
    if len(encoded) > _MAX_EXPORT_BYTES:
        sources.fail("SOURCE_LIMIT", "Complete raw export exceeds its managed byte limit.")
    identity = hashlib.sha256(encoded).hexdigest()
    export_id = str(uuid.uuid4())
    manifest = {"schemaId": "range.series-export/v1", "methodVersion": METHOD_VERSION,
                "exportId": export_id, "scope": "complete-raw", "sha256": identity,
                "bytes": len(encoded), "count": count,
                "sources": [copy.deepcopy(r["metadata"]) for r in resolved]}
    if hasattr(service, "_read_source_gate"):
        service._read_source_gate()
    with _EXPORT_LOCK:
        registry = _exports(service)
        if len(registry) >= 16:
            sources.fail("SOURCE_LIMIT", "Managed exports have reached their sixteen-artifact limit.")
        root = _export_root(service, create=True)
        if sum(1 for p in root.iterdir() if p.suffix == ".csv") >= 16:
            sources.fail("SOURCE_LIMIT", "Managed export directory has reached its sixteen-artifact limit.")
        path = root / (export_id + ".csv")
        # Reserve the exact filename while holding the quota lock. Large I/O
        # and hashes cannot block admission of unrelated reads or controls.
        try:
            stream = path.open("xb")
        except OSError:
            sources.fail("EXPORT_FAILED", "Managed export filename could not be reserved.")
        created_identity = os.fstat(stream.fileno())
    try:
        with stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        if file_hash(path) != identity:
            raise OSError()
        _export_root(service)
        if path.is_symlink() or path.resolve(strict=True).parent != root.resolve(strict=True):
            raise OSError()
        if hasattr(service, "_read_source_gate"):
            service._read_source_gate()
        with _EXPORT_LOCK:
            registry[export_id] = copy.deepcopy(manifest)
    except (OSError, WorkflowError) as failure:
        # Only this invocation created this opaque filename. Never remove an
        # existing export or follow a replaced path during failure cleanup.
        try:
            current_identity = path.lstat()
            if (not path.is_symlink() and path.parent.resolve(strict=True) == root.resolve(strict=True)
                    and (current_identity.st_dev, current_identity.st_ino) ==
                        (created_identity.st_dev, created_identity.st_ino)):
                path.unlink(missing_ok=True)
        except OSError:
            pass
        if isinstance(failure, WorkflowError):
            raise
        sources.fail("EXPORT_FAILED", "Managed complete raw export could not be published.")
    return copy.deepcopy(manifest)


def _chunk(service, parameters):
    exact(parameters, {"exportId", "offset", "length"})
    export_id = identifier(parameters["exportId"], "exportId")
    offset = integer(parameters["offset"], "offset", 0, _MAX_EXPORT_BYTES)
    length = integer(parameters["length"], "length", 1, 65536)
    with _EXPORT_LOCK:
        manifest = copy.deepcopy(_exports(service).get(export_id))
    if manifest is None:
        sources.fail("EXPORT_UNAVAILABLE", "Managed export identity is unavailable.")
    root = _export_root(service)
    path = root / (export_id + ".csv")
    try:
        if (path.is_symlink() or not path.is_file() or path.resolve(strict=True).parent != root.resolve(strict=True)
                or path.stat().st_size != manifest["bytes"] or file_hash(path) != manifest["sha256"]):
            raise ValueError()
        if offset > manifest["bytes"]:
            sources.fail("INVALID_PARAMETERS", "Chunk offset exceeds the complete export size.")
        with path.open("rb") as stream:
            stream.seek(offset)
            data = stream.read(min(length, manifest["bytes"] - offset))
        if (len(data) != min(length, manifest["bytes"] - offset) or file_hash(path) != manifest["sha256"]
                or path.is_symlink() or path.resolve(strict=True).parent != _export_root(service).resolve(strict=True)):
            raise ValueError()
    except WorkflowError:
        raise
    except (OSError, ValueError, RuntimeError):
        sources.fail("SOURCE_CHANGED", "Managed raw export bytes or path changed.")
    return {"schemaId": "range.series-export-chunk/v1", "exportId": export_id,
            "sha256": manifest["sha256"], "bytes": manifest["bytes"], "offset": offset,
            "length": len(data), "encoding": "base64", "data": base64.b64encode(data).decode("ascii"),
            "complete": offset + len(data) == manifest["bytes"]}


def handle(service, operation, parameters):
    if operation == "result.plot.query":
        return _plot(service, parameters)
    if operation == "result.raw.query":
        return _raw(service, parameters)
    if operation == "result.raw.export":
        return _export(service, parameters)
    if operation == "result.raw.export.chunk":
        return _chunk(service, parameters)
    sources.fail("UNKNOWN_OPERATION", "Unsupported series workbench operation.")
