"""Host-owned scientific v2 lifecycle helpers; generated UI/code has no authority."""
from __future__ import annotations

import copy
import hashlib
import math
from pathlib import Path
import re

from .service import WorkflowError, canonical, digest, exact, identifier, file_hash, utc
from . import scientific_contract as contract
from . import scientific_reference as reference
from .scientific_runtime import runtime_identity

PROTOCOL_VERSION = "range.extension/v2"
KIND = "multichannel-spectral-filter"
MANIFEST_SCHEMA = "range.extension/v2/manifest"
GUI = {
    "form": [{"id": "sources", "type": "channel-select", "minimum": 1, "maximum": 4},
             {"id": "window", "type": "enum", "values": ["rectangular", "hann"]},
             {"id": "removeMean", "type": "enum", "values": [False, True]},
             {"id": "filter", "type": "enum", "values": ["none", "butterworth-lowpass"]},
             {"id": "cutoffHz", "type": "number", "unit": "Hz"}],
    "table": {"columns": ["channel", "unit", "sampleCount", "binCount"]},
    "plots": ["filtered-time", "amplitude", "psd", "real", "imag", "phase"],
}


def checked(function, *args, **kwargs):
    try:
        return function(*args, **kwargs)
    except contract.ScientificContractError as error:
        raise WorkflowError("SCIENTIFIC_CONTRACT", str(error)) from None


def validate_manifest(value):
    exact(value, {"manifestVersion", "extensionId", "version", "sdkVersion", "kind", "entryPoint",
                  "compatibility", "dependencies", "capabilities", "inputSchema", "outputSchema", "gui"})
    if (type(value["manifestVersion"]) is not int or value["manifestVersion"] != 2
            or value["sdkVersion"] != PROTOCOL_VERSION or value["kind"] != KIND
            or value["entryPoint"] != "extension.py:process"
            or value["compatibility"] != {"workflowApi": "range.workflow/v1", "inputVersion": 2, "outputVersion": 2}
            or value["inputSchema"] != contract.INPUT_SCHEMA or value["outputSchema"] != contract.OUTPUT_SCHEMA):
        raise WorkflowError("UNSUPPORTED_VERSION", "Unsupported scientific v2 manifest")
    dependencies = [{"name": name, "version": contract.DEPENDENCIES[name]} for name in ("numpy", "scipy")]
    if value["dependencies"] != dependencies:
        raise WorkflowError("UNSUPPORTED_DEPENDENCY", "Only frozen NumPy/SciPy dependencies are supported")
    if value["capabilities"] != ["numeric.ndarray.real64", "numeric.fft.rfft", "signal.lowpass1"]:
        raise WorkflowError("UNSUPPORTED_CAPABILITY", "No trusted oracle exists for requested scientific capabilities")
    if canonical(value["gui"]) != canonical(GUI):
        raise WorkflowError("INVALID_PARAMETERS", "Scientific GUI must use the supported declarative controls")
    identifier(value["extensionId"], "extensionId")
    if (not isinstance(value["version"], str) or len(value["version"]) > 64
            or re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[A-Za-z0-9.-]{1,32})?", value["version"]) is None):
        raise WorkflowError("INVALID_PARAMETERS", "version must be a semantic version")
    return copy.deepcopy(value)


def validate_cases(value):
    exact(value, {"cases"})
    if type(value["cases"]) is not list or not 1 <= len(value["cases"]) <= 4:
        raise WorkflowError("INVALID_PARAMETERS", "Scientific candidate tests require 1..4 cases")
    normalized, seen = [], set()
    for case in value["cases"]:
        exact(case, {"id", "input", "expected"})
        ident = identifier(case["id"], "test id")
        if ident in seen:
            raise WorkflowError("INVALID_PARAMETERS", "Duplicate scientific test identity")
        seen.add(ident)
        data = checked(contract.validate_input, case["input"])
        if len(data["times"]) > 32:
            raise WorkflowError("EXTENSION_INPUT_LIMIT", "Candidate validation fixtures are bounded to 32 samples")
        expected = checked(contract.validate_output, case["expected"], data)
        normalized.append({"id": ident, "input": data, "expected": expected})
    return {"cases": normalized}


def golden_cases():
    cases = []
    for ident, n, window, mean, filt in (
            ("host-dc-even", 8, "rectangular", False, {"kind": "none"}),
            ("host-phase-odd", 9, "rectangular", False, {"kind": "none"}),
            ("host-hann-multichannel", 16, "hann", True, {"kind": "none"}),
            ("host-lowpass-zero-ic", 16, "rectangular", False,
             {"kind": "butterworth-lowpass", "order": 1, "cutoffHz": 2., "phase": "causal", "initialCondition": "zero"})):
        channels = [{"id": "velocity", "quantity": "velocity", "axis": "x", "unit": "m/s",
                     "values": [2. if ident == "host-dc-even" else math.sin(2*math.pi*2*j/n+.3) + .2*(-1)**j for j in range(n)]},
                    {"id": "force", "quantity": "force", "axis": "x", "unit": "N",
                     "values": [1. if j == 0 else 0. for j in range(n)]}]
        data = checked(contract.validate_input, {"schema": contract.INPUT_SCHEMA, "algorithm": contract.ALGORITHM,
            "domain": "time", "scalarType": "real64", "times": [j/16 for j in range(n)], "timeUnit": "s",
            "samplePeriodS": 1/16, "samplingRateHz": 16., "channels": channels,
            "window": window, "removeMean": mean, "filter": filt})
        cases.append({"id": ident, "input": data, "expected": None})
    return cases


def execute_source(service, directory, record, data, cancel, *, tests=None):
    from .extensions import _safe_relative, _copy_verified, _safe_write, _clean_environment, MAX_SOURCE_BYTES
    if not service.trusted_extensions:
        raise WorkflowError("TRUSTED_RUNTIME_REQUIRED", "Scientific execution requires trusted developer mode")
    runtime = runtime_identity(service, force_verify=True)
    if runtime["runtimeHash"] != record["runtimeHash"]:
        raise WorkflowError("RUNTIME_CHANGED", "Pinned scientific runtime changed")
    source = _safe_relative(service, record["sourcePath"], record["sourceSha256"], MAX_SOURCE_BYTES)
    copied = directory / "extension.py"
    _copy_verified(source, copied, record["sourceSha256"], MAX_SOURCE_BYTES)
    supplied = tests if tests is not None else checked(contract.validate_input, data)
    raw = canonical(supplied)
    if len(raw) > contract.MAX_JSON_BYTES:
        raise WorkflowError("EXTENSION_INPUT_LIMIT", "Scientific input exceeds 32 MiB")
    input_path, output = directory / "input.json", directory / "scientific-result.json"
    _safe_write(input_path, raw)
    child = Path(__file__).with_name("extension_child_v2.py").resolve()
    service.runner.run([str(service.python), "-I", "-B", str(child), "--source", str(copied),
        "--input", str(input_path), "--output", str(output), "--test" if tests is not None else "--run"],
        directory, "scientific-child", cancel, timeout=service.extension_timeout_seconds,
        env=_clean_environment(directory))
    # Complete byte hashes bracket the whole generated/oracle batch. Here the
    # full file-set/stat check still rejects changes before host verification.
    if runtime_identity(service) != runtime:
        raise WorkflowError("RUNTIME_CHANGED", "Scientific environment changed during execution")
    if (output.is_symlink() or not output.is_file() or output.stat().st_size > contract.MAX_JSON_BYTES
            or file_hash(copied) != record["sourceSha256"] or file_hash(input_path) != hashlib.sha256(raw).hexdigest()):
        raise WorkflowError("EXTENSION_TAMPERED", "Scientific output is oversized or approved input was modified")
    raw_output = output.read_bytes()
    if len(raw_output) > contract.MAX_JSON_BYTES:
        raise WorkflowError("EXTENSION_OUTPUT_LIMIT", "Scientific output changed size during extraction")
    return checked(contract.strict_json, raw_output), hashlib.sha256(raw_output).hexdigest()


def resolve_input(service, sources, options):
    from .series_sources import resolve_many
    exact(options, {"window", "removeMean", "filter"})
    if type(sources) is not list or not 1 <= len(sources) <= 4:
        raise WorkflowError("INVALID_PARAMETERS", "Scientific input requires 1..4 verified channels")
    resolved = resolve_many(service, sources)
    times = [s["time"] for s in resolved[0]["dto"]["samples"]]
    if len(times) < 2:
        raise WorkflowError("INVALID_PARAMETERS", "Scientific input requires at least two accepted samples")
    channels, provenance = [], []
    for i, item in enumerate(resolved):
        dto, metadata = item["dto"], item["metadata"]
        if [s["time"] for s in dto["samples"]] != times:
            raise WorkflowError("INCOMPARABLE_RESULTS", "Scientific channels have different accepted clocks")
        identity = metadata["channel"]
        quantity = "energy" if metadata["selector"]["kind"] == "energy" and identity["quantity"] == "mechanicalEnergy" else identity["quantity"]
        channels.append({"id": f"channel-{i+1}", "quantity": quantity,
            "axis": identity["axis"].lower(), "unit": dto["unit"],
            "values": [s["value"] for s in dto["samples"]],
            "source": {"sourceHash": dto["sourceHash"], "artifactId": dto["artifactId"], "channelId": dto["channelId"]}})
        provenance.append(metadata)
    dt = times[1]-times[0]
    data = checked(contract.validate_input, {"schema": contract.INPUT_SCHEMA, "algorithm": contract.ALGORITHM,
        "domain": "time", "scalarType": "real64", "times": times, "timeUnit": "s",
        "samplePeriodS": dt, "samplingRateHz": 1/dt, "channels": channels, **options})
    return data, {"sourceKind": "verified-multichannel", "sources": provenance,
                  "options": options, "inputHash": contract.canonical_hash(data)}


def execute_validate(service, job, directory, cancel, record, manifest, candidate_tests):
    from .extensions import _safe_write
    p = job["parameters"]
    cases = candidate_tests["cases"] + golden_cases()
    results, generated_sha = execute_source(service, directory, {**record, "runtimeHash": p["runtime"]["runtimeHash"]}, {}, cancel,
                             tests={"cases": [{"id": c["id"], "input": c["input"]} for c in cases]})
    exact(results, {"results"})
    if type(results["results"]) is not list or len(results["results"]) != len(cases):
        raise WorkflowError("EXTENSION_TEST_FAILED", "Scientific validation case count differs")
    oracle_results, oracle_hash = private_oracle_batch(service, directory, cases, cancel)
    receipts = []
    for case, result, oracle in zip(cases, results["results"], oracle_results):
        exact(result, {"id", "output"})
        if result["id"] != case["id"]:
            raise WorkflowError("EXTENSION_TEST_FAILED", "Scientific validation case identity differs")
        if oracle["id"] != case["id"]:
            raise WorkflowError("EXTENSION_TEST_FAILED", "Private oracle case identity differs")
        profile = checked(reference.compare_results, result["output"], oracle["output"], case["input"])
        if case["expected"] is not None:
            checked(reference.compare_results, case["expected"], oracle["output"], case["input"])
        receipts.append({"id": case["id"], "passed": True,
                         "inputHash": contract.canonical_hash(case["input"]),
                         "outputHash": contract.canonical_hash(result["output"]),
                         "verificationProfile": profile, "oracleBatchSha256": oracle_hash})
    if runtime_identity(service, force_verify=True) != p["runtime"]:
        raise WorkflowError("RUNTIME_CHANGED", "Scientific environment changed during independent validation")
    if file_hash(directory / "scientific-result.json") != generated_sha:
        raise WorkflowError("EXTENSION_TAMPERED", "Scientific validation output changed during host verification")
    report = {"protocolVersion": PROTOCOL_VERSION, "candidateId": record["candidateId"],
        "bundleHash": record["bundleHash"], "runtimeHash": p["runtime"]["runtimeHash"],
        "contractHash": contract.CONTRACT_HASH, "oracleSha256": file_hash(Path(reference.__file__)),
        "caseCount": len(candidate_tests["cases"]), "independentCaseCount": 4,
        "passed": True, "cases": receipts}
    report_path = directory / "validation-report.json"
    _safe_write(report_path, canonical(report))
    validation = {"passed": True, "caseCount": len(candidate_tests["cases"]), "independentCaseCount": 4,
        "reportPath": report_path.relative_to(service.workspace).as_posix(), "reportSha256": file_hash(report_path),
        "runtimeHash": p["runtime"]["runtimeHash"], "validatedAt": utc()}
    return {"extensionUpdate": {"operation": "candidate.validate", "extensionId": record["extensionId"],
        "expectedRegistryRevision": p["registryRevision"], "candidateId": record["candidateId"],
        "bundleHash": record["bundleHash"], "validation": validation},
        "extensionId": record["extensionId"], "version": manifest["version"],
        "candidateId": record["candidateId"], "bundleHash": record["bundleHash"], "state": "validated",
        "runtimeHash": validation["runtimeHash"], "reportHash": validation["reportSha256"],
        "validation": {k: validation[k] for k in ("passed", "caseCount", "independentCaseCount")}}


def normalize_run(service, parameters, version):
    from .extensions import _registry_entry
    exact(parameters, {"extensionId", "bundleHash", "sources", "options"})
    entry = _registry_entry(service, parameters["extensionId"])
    if entry["activeBundleHash"] != parameters["bundleHash"]:
        raise WorkflowError("EXTENSION_NOT_ACTIVE", "Scientific run requires the current active bundle")
    data, provenance = resolve_input(service, parameters["sources"], parameters["options"])
    return {"sdkVersion": PROTOCOL_VERSION, "extensionId": parameters["extensionId"],
        "bundleHash": parameters["bundleHash"], "series": data, "provenance": provenance,
        "version": version["version"], "runtimeHash": runtime_identity(service)["runtimeHash"]}


def revalidate_input(service, p):
    selectors = [s["selector"] for s in p["provenance"]["sources"]]
    data, provenance = resolve_input(service, selectors, p["provenance"]["options"])
    if data != p["series"] or provenance != p["provenance"]:
        raise WorkflowError("SOURCE_CHANGED", "Scientific input or source proof changed since preview")
    return data


def bind_run(service, p):
    from .extensions import _version, _source_record, _safe_relative, MAX_REPORT_BYTES
    entry, version = _version(service, p["extensionId"], p["bundleHash"])
    if entry["activeBundleHash"] != p["bundleHash"]:
        raise WorkflowError("EXTENSION_NOT_ACTIVE", "Scientific preview refers to an inactive version")
    _source_record(service, version)
    validation = version["validation"]
    _safe_relative(service, validation["reportPath"], validation["reportSha256"], MAX_REPORT_BYTES)
    runtime = runtime_identity(service)
    if runtime["runtimeHash"] != p["runtimeHash"] or runtime["runtimeHash"] != version["runtimeHash"]:
        raise WorkflowError("RUNTIME_CHANGED", "Scientific active version requires revalidation")
    data = revalidate_input(service, p)
    return {"extensionId": p["extensionId"], "bundleHash": p["bundleHash"], "version": version["version"],
        **{k: version[k] for k in ("manifestSha256", "sourceSha256", "testsSha256", "sourcePath")},
        "reportSha256": validation["reportSha256"], "runtimeHash": runtime["runtimeHash"],
        "seriesSha256": contract.canonical_hash(data), "provenance": p["provenance"]}


def execute_run(service, job, directory, cancel):
    from .extensions import _version, _source_record, _safe_write
    p, binding = job["parameters"], job["_extensionBinding"]
    _, version = _version(service, p["extensionId"], p["bundleHash"])
    _source_record(service, version)
    if any(version[k] != binding[k] for k in ("sourceSha256", "sourcePath", "manifestSha256", "testsSha256")):
        raise WorkflowError("EXTENSION_TAMPERED", "Pinned scientific bundle changed")
    data = revalidate_input(service, p)
    if contract.canonical_hash(data) != binding["seriesSha256"]:
        raise WorkflowError("SOURCE_CHANGED", "Pinned scientific input changed")
    output, generated_sha = execute_source(service, directory, {**version, "runtimeHash": binding["runtimeHash"]}, data, cancel)
    revalidate_input(service, p)
    profile = verify_private(service, directory, output, data, cancel, "actual-run")
    if runtime_identity(service, force_verify=True)["runtimeHash"] != binding["runtimeHash"]:
        raise WorkflowError("RUNTIME_CHANGED", "Scientific runtime changed during complete verification")
    output_path = directory / "scientific-result.json"
    if output_path.is_symlink() or not output_path.is_file() or file_hash(output_path) != generated_sha:
        raise WorkflowError("EXTENSION_TAMPERED", "Scientific result changed during host verification")
    output_hash = file_hash(output_path)
    if output_hash != generated_sha:
        raise WorkflowError("EXTENSION_TAMPERED", "Scientific result changed while publishing its receipt")
    provenance_raw = canonical({"provenance": p["provenance"],
        "seriesSha256": binding["seriesSha256"], "bundleHash": p["bundleHash"], "runtimeHash": binding["runtimeHash"]})
    _safe_write(directory / "input-provenance.json", provenance_raw)
    return {"sdkVersion": PROTOCOL_VERSION, "extensionId": p["extensionId"], "version": binding["version"],
        "bundleHash": p["bundleHash"], "runtimeHash": binding["runtimeHash"],
        "sourceKind": "verified-multichannel", "seriesSha256": binding["seriesSha256"],
        "inputProvenanceSha256": hashlib.sha256(provenance_raw).hexdigest(),
        "verificationProfile": profile, "contractHash": contract.CONTRACT_HASH,
        "scientificResult": {"path": output_path.relative_to(service.workspace).as_posix(),
                             "sha256": output_hash, "bytes": output_path.stat().st_size,
                             "sampleCount": output["sampleCount"], "binCount": output["binCount"],
                             "channels": [{k: c[k] for k in ("id", "quantity", "axis", "unit")} for c in output["channels"]]}}


def verify_private(service, directory, actual, data, cancel, name):
    from .extensions import _safe_write, _clean_environment
    checked(contract.validate_output, actual, data)
    # Each case checks the complete file-set/stat fingerprint. Full byte hashes
    # bracket the entire generated-child and host validation/run batch.
    before = runtime_identity(service)
    input_path, output_path = directory / (name + "-oracle-input.json"), directory / (name + "-oracle-output.json")
    raw = canonical(data)
    _safe_write(input_path, raw)
    service.runner.run([str(service.python), "-I", "-B", str(Path(__file__).with_name("scientific_oracle_child.py")),
        "--input", str(input_path), "--output", str(output_path)], directory, name + "-oracle", cancel,
        timeout=service.extension_timeout_seconds, env=_clean_environment(directory))
    if runtime_identity(service) != before or file_hash(input_path) != hashlib.sha256(raw).hexdigest():
        raise WorkflowError("RUNTIME_CHANGED", "Private oracle environment/input changed")
    if output_path.is_symlink() or not output_path.is_file() or output_path.stat().st_size > contract.MAX_JSON_BYTES:
        raise WorkflowError("EXTENSION_OUTPUT_LIMIT", "Private oracle output is unavailable or oversized")
    oracle_raw = output_path.read_bytes()
    oracle_sha = hashlib.sha256(oracle_raw).hexdigest()
    expected = checked(contract.strict_json, oracle_raw)
    checked(contract.validate_output, expected, data)
    profile = checked(reference.compare_results, actual, expected, data)
    if output_path.is_symlink() or file_hash(output_path) != oracle_sha:
        raise WorkflowError("EXTENSION_TAMPERED", "Oracle output changed during verification")
    return {**profile, "runtimeHash": before["runtimeHash"], "oracleOutputSha256": oracle_sha}


def private_oracle_batch(service, directory, cases, cancel):
    from .extensions import _safe_write, _clean_environment
    before = runtime_identity(service)
    input_path, output_path = directory / "oracle-batch-input.json", directory / "oracle-batch-output.json"
    raw = canonical({"cases": [{"id": c["id"], "input": c["input"]} for c in cases]})
    _safe_write(input_path, raw)
    service.runner.run([str(service.python), "-I", "-B", str(Path(__file__).with_name("scientific_oracle_child.py")),
        "--input", str(input_path), "--output", str(output_path), "--test"], directory, "oracle-batch", cancel,
        timeout=service.extension_timeout_seconds, env=_clean_environment(directory))
    if runtime_identity(service) != before or file_hash(input_path) != hashlib.sha256(raw).hexdigest():
        raise WorkflowError("RUNTIME_CHANGED", "Oracle batch environment/input changed")
    if output_path.is_symlink() or not output_path.is_file() or output_path.stat().st_size > contract.MAX_JSON_BYTES:
        raise WorkflowError("EXTENSION_OUTPUT_LIMIT", "Oracle batch output exceeds bound")
    oracle_raw = output_path.read_bytes()
    oracle_sha = hashlib.sha256(oracle_raw).hexdigest()
    results = checked(contract.strict_json, oracle_raw)
    exact(results, {"results"})
    if type(results["results"]) is not list or len(results["results"]) != len(cases):
        raise WorkflowError("EXTENSION_TEST_FAILED", "Oracle batch case count differs")
    if output_path.is_symlink() or file_hash(output_path) != oracle_sha:
        raise WorkflowError("EXTENSION_TAMPERED", "Oracle batch output changed during extraction")
    return results["results"], oracle_sha


def result_query(service, parameters):
    from .extensions import _safe_relative
    exact(parameters, {"jobId"}, {"channel", "offset", "limit", "view"})
    job = service._job(parameters["jobId"])
    result = job.get("result", {})
    if job.get("state") != "succeeded" or result.get("sdkVersion") != PROTOCOL_VERSION:
        raise WorkflowError("RESULT_UNAVAILABLE", "Successful scientific v2 result required")
    artifact = result["scientificResult"]
    if artifact["path"] != f"jobs/{job['jobId']}/scientific-result.json":
        raise WorkflowError("SOURCE_CHANGED", "Scientific result ownership differs")
    path = _safe_relative(service, artifact["path"], artifact["sha256"], contract.MAX_JSON_BYTES)
    from .service import integer
    channel = integer(parameters.get("channel", 0), "channel", 0, len(artifact["channels"])-1)
    view = parameters.get("view", "amplitude")
    if view not in ("filtered-time", "amplitude", "psd", "real", "imag", "phase"):
        raise WorkflowError("INVALID_PARAMETERS", "Unknown scientific plot view")
    count = artifact["sampleCount"] if view == "filtered-time" else artifact["binCount"]
    offset = integer(parameters.get("offset", 0), "offset", 0, count)
    limit = integer(parameters.get("limit", 256), "limit", 1, 1000)
    value = checked(contract.strict_json, path.read_bytes())
    if file_hash(path) != artifact["sha256"]:
        raise WorkflowError("SOURCE_CHANGED", "Scientific result changed during extraction")
    c = value["channels"][channel]
    provenance_path = _safe_relative(service, f"jobs/{job['jobId']}/input-provenance.json",
                                    result["inputProvenanceSha256"], 256*1024)
    provenance = checked(contract.strict_json, provenance_path.read_bytes())
    if any(provenance.get(key) != result[key] for key in ("seriesSha256", "bundleHash", "runtimeHash")):
        raise WorkflowError("SOURCE_CHANGED", "Scientific input provenance differs from the result")
    end = min(offset+limit, count)
    x = value["times"] if view == "filtered-time" else value["frequenciesHz"]
    y = (c["filteredValues"] if view == "filtered-time" else c["amplitude"] if view == "amplitude"
         else c["psd"] if view == "psd" else [pair[0] for pair in c["fft"]] if view == "real"
         else [pair[1] for pair in c["fft"]] if view == "imag"
         else [math.atan2(pair[1], pair[0]) for pair in c["fft"]])
    y_unit = "rad" if view == "phase" else (c["unit"] + "^2/Hz" if view == "psd" else c["unit"])
    return {"sdkVersion": PROTOCOL_VERSION, "jobId": job["jobId"], "bundleHash": result["bundleHash"],
        "runtimeHash": result["runtimeHash"], "sourceSha256": result["seriesSha256"],
        "resultSha256": artifact["sha256"], "verificationProfile": result["verificationProfile"],
        "inputSources": provenance["provenance"]["sources"], "inputOptions": provenance["provenance"]["options"],
        "channel": artifact["channels"][channel], "channelCount": len(artifact["channels"]),
        "sampleCount": artifact["sampleCount"], "binCount": artifact["binCount"],
        "offset": offset, "nextOffset": end if end < count else None,
        "view": view, "domain": "time" if view == "filtered-time" else "frequency",
        "xUnit": "s" if view == "filtered-time" else "Hz", "xLabel": "Time" if view == "filtered-time" else "Frequency",
        "yUnit": y_unit, "yLabel": view, "sourceHash": artifact["sha256"], "pointCount": count,
        "points": [{"x": x[i], "y": y[i], "rawOrdinal": i} for i in range(offset, end)],
        "frequencyUnit": "Hz", "psdUnit": c["psdUnit"],
        "frequenciesHz": value["frequenciesHz"][offset:min(end, artifact["binCount"])],
        **{key: c[key][offset:min(end, artifact["binCount"])] for key in ("fft", "amplitude", "psd")}}


def schema_get(schema_id):
    if schema_id not in (MANIFEST_SCHEMA, contract.INPUT_SCHEMA, contract.OUTPUT_SCHEMA):
        raise WorkflowError("SCHEMA_NOT_FOUND", "Unknown scientific v2 schema")
    from .scientific_schemas import schemas
    return {"schemaId": schema_id, "apiVersion": PROTOCOL_VERSION,
        "contractHash": contract.CONTRACT_HASH, "verificationProfile": contract.verification_profile(),
        "schema": {**schemas()[schema_id], "supportedGui": GUI}}
