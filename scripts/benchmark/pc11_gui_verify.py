"""Join actual installed-GUI paint/commit, UIA, CSV and memory evidence.

This does not run a solver or generate data. QUALIFIED applies to one family and
one run only. The frozen PC11 gates are 5 s, 30 s and physical RAM used below 90%.
"""
import argparse
import base64
import csv
from datetime import datetime
import hashlib
import io
import json
import math
import os
from pathlib import Path
import time


GUI_EVENT_SCHEMA = "range.piezo-gui-performance-event/v1"
COMPACT_TRACE_METADATA_SCHEMA = "range.piezo-accepted-trace-timeseries/v1"
LEGACY_TIMESERIES_METADATA_SCHEMA = "range.piezo-accepted-timeseries/v1"
MANIFEST_FAMILIES = {"axialRod", "planarBeam", "flatPlate"}
COMPACT_FAMILY_TO_MANIFEST = {"axial": "axialRod"}
KERNEL_FAMILY_TO_MODEL_FAMILIES = {
    "axial": {"axialRod"},
    "layered": {"planarBeam", "flatPlate"},
}
PHYSICAL_RAM_SAMPLE_COLUMNS = [
    "monotonic_ns", "elapsed_seconds", "total_physical_bytes",
    "available_physical_bytes", "used_percent",
]
PHYSICAL_RAM_LIMIT_PERCENT = 90.0
PHYSICAL_RAM_MAX_SAMPLE_GAP_MILLISECONDS = 250.0


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def same_path(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def read_lines(path):
    with path.open(encoding="utf-8-sig") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def one(values, condition, description):
    found = [v for v in values if condition(v)]
    require(len(found) == 1, "Expected exactly one " + description)
    return found[0]


def stamp(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def check_event(event, pid, executable):
    require(event.get("schema") == GUI_EVENT_SCHEMA, "Event schema mismatch")
    require(int(event["guiPid"]) == pid and same_path(event["guiExecutable"], executable), "Event process mismatch")
    require(event.get("measurementValid") is True and event.get("requestRoute") == "product-native-path", "Fixture/invalid event cannot qualify")
    require(event["event"] != "request_superseded", "Measured plot was superseded")


def check_performance_family(event, manifest_family):
    require(event.get("schema") == GUI_EVENT_SCHEMA, "Event schema mismatch")
    require(event.get("requestRoute") == "product-native-path", "Fixture/invalid event cannot qualify")
    require(manifest_family in MANIFEST_FAMILIES, "Unknown manifest family")
    require(compact_manifest_family(event) == manifest_family, "Wrong family")


def compact_manifest_family(value):
    kernel_family = value.get("family")
    require(isinstance(kernel_family, str) and kernel_family in KERNEL_FAMILY_TO_MODEL_FAMILIES, "Unknown compact kernel family")
    if "modelFamily" not in value:
        require(kernel_family in COMPACT_FAMILY_TO_MANIFEST, "Typed modelFamily required for layered compact family")
        return COMPACT_FAMILY_TO_MANIFEST[kernel_family]
    model_family = value["modelFamily"]
    require(isinstance(model_family, str) and model_family in MANIFEST_FAMILIES, "Unknown or malformed modelFamily")
    require(model_family in KERNEL_FAMILY_TO_MODEL_FAMILIES[kernel_family], "Kernel/model family mismatch")
    return model_family


def manifest_family_from_metadata(metadata):
    schema = metadata.get("schema")
    if schema == COMPACT_TRACE_METADATA_SCHEMA:
        return compact_manifest_family(metadata)
    if schema == LEGACY_TIMESERIES_METADATA_SCHEMA:
        family = metadata.get("family")
        require(isinstance(family, str) and family in MANIFEST_FAMILIES, "Unknown legacy CSV family")
        if "modelFamily" in metadata:
            model_family = metadata["modelFamily"]
            require(isinstance(model_family, str) and model_family in MANIFEST_FAMILIES, "Unknown or malformed legacy modelFamily")
            require(model_family == family, "Legacy family/modelFamily mismatch")
        return family
    raise ValueError("CSV metadata schema mismatch")


def check_complete_metadata(metadata, family):
    require(family in MANIFEST_FAMILIES, "Unknown manifest family")
    require(manifest_family_from_metadata(metadata) == family, "Wrong family")
    require(metadata["recordCount"] == 20001 and metadata["targetSteps"] == 20000 and metadata["baseSeq"] == 0, "Incomplete CSV metadata")
    require(metadata["continuousBetweenSelectedRecords"] is True, "CSV has nonconsecutive FE steps")


def check_physical_ram(directory, gui_start_ns, baseline_ns, begin_ns, end_ns):
    physical_dir = directory / "physical-ram"
    summary_path = physical_dir / "summary.json"
    samples_path = physical_dir / "physical-ram.csv"
    require(summary_path.is_file() and samples_path.is_file(), "Physical RAM series missing")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    require(summary.get("schema") == "range.pc11-physical-ram/v1" and summary.get("status") == "MEASURED" and
            summary.get("source") == "GlobalMemoryStatusEx", "Physical RAM measurement unavailable")
    require(summary.get("samplingIntervalMilliseconds") == 50, "Wrong physical RAM sampling interval")

    samples = []
    with samples_path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        require(reader.fieldnames == PHYSICAL_RAM_SAMPLE_COLUMNS, "Physical RAM series columns mismatch")
        for row in reader:
            monotonic_ns = int(row["monotonic_ns"])
            elapsed_seconds = float(row["elapsed_seconds"])
            total_bytes = int(row["total_physical_bytes"])
            available_bytes = int(row["available_physical_bytes"])
            used_percent = float(row["used_percent"])
            require(monotonic_ns > 0 and math.isfinite(elapsed_seconds) and elapsed_seconds >= 0.0,
                    "Invalid physical RAM sample time")
            require(total_bytes > 0 and 0 <= available_bytes <= total_bytes,
                    "Invalid physical RAM sample bytes")
            calculated_percent = 100.0 * (1.0 - available_bytes / total_bytes)
            require(math.isfinite(used_percent) and 0.0 <= used_percent <= 100.0 and
                    math.isclose(used_percent, calculated_percent, rel_tol=0.0, abs_tol=0.00001),
                    "Physical RAM sample percent differs from OS byte counts")
            samples.append({"monotonicNs": monotonic_ns, "elapsedSeconds": elapsed_seconds,
                            "totalBytes": total_bytes, "usedPercent": used_percent})

    require(len(samples) >= 10 and int(summary.get("sampleCount", -1)) == len(samples),
            "Insufficient or incomplete physical RAM series")
    times = [sample["monotonicNs"] for sample in samples]
    require(all(right > left for left, right in zip(times, times[1:])), "Physical RAM sample times are not increasing")
    gaps = [(right - left) / 1e6 for left, right in zip(times, times[1:])]
    maximum_gap = max(gaps, default=0.0)
    maximum_used = max(sample["usedPercent"] for sample in samples)
    covered_ms = (times[-1] - times[0]) / 1e6
    require(maximum_gap <= PHYSICAL_RAM_MAX_SAMPLE_GAP_MILLISECONDS,
            "Physical RAM series has an incomplete sampling gap")
    require(int(summary.get("firstSampleMonotonicNs", -1)) == times[0] and
            int(summary.get("lastSampleMonotonicNs", -1)) == times[-1],
            "Physical RAM summary timestamps differ from its series")
    require(int(summary.get("totalPhysicalBytes", -1)) == samples[0]["totalBytes"],
            "Physical RAM summary total differs from its series")
    require(math.isclose(float(summary.get("maximumUsedPercent", math.nan)), maximum_used,
                         rel_tol=0.0, abs_tol=0.00001),
            "Physical RAM maximum differs from its series")
    require(math.isclose(float(summary.get("maximumSampleGapMilliseconds", math.nan)), maximum_gap,
                         rel_tol=0.0, abs_tol=0.001) and
            math.isclose(float(summary.get("coveredDurationMilliseconds", math.nan)), covered_ms,
                         rel_tol=0.0, abs_tol=0.001),
            "Physical RAM summary coverage differs from its series")
    require(times[0] <= begin_ns + 10_000_000 and times[-1] >= end_ns - 10_000_000,
            "Physical RAM series did not cover the complete GUI requests")
    require(gui_start_ns - 10_000_000 <= times[0] <= gui_start_ns + 100_000_000 and
            times[0] <= baseline_ns + 10_000_000,
            "Physical RAM series did not start with the GUI run")
    return {"sampleCount": len(samples), "maximumUsedPercent": maximum_used,
            "maximumSampleGapMilliseconds": maximum_gap, "coveredDurationMilliseconds": covered_ms,
            "source": summary["source"]}


def working_set_diagnostic(directory, pid, executable, begin_ns, end_ns):
    """Return process-tree working set as optional diagnostic evidence."""
    summary_path = directory / "working-set" / "summary.json"
    if not summary_path.is_file():
        return {"status": "UNAVAILABLE", "maximumWorkingSetBytes": None,
                "maximumSampleGapMilliseconds": None, "error": "Working-set summary missing"}
    try:
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        require(isinstance(summary, dict), "Working-set summary must be an object")
        require(summary.get("status") == "MEASURED", "Working-set sampler did not produce a measurement")
        require(int(summary["rootPid"]) == pid and same_path(summary["expectedRootImage"], executable),
                "Working-set process identity mismatch")
        require(summary["samplingIntervalMilliseconds"] == 50 and summary["includeBaseline"] is True and
                summary["subtractSharedLibraries"] is False, "Wrong working-set diagnostic metric")
        require(int(summary["sampleCount"]) >= 10 and int(summary["maximumWorkingSetBytes"]) >= 0,
                "Insufficient working-set diagnostic samples")
        first_ns = int(summary["firstSampleMonotonicNs"])
        last_ns = int(summary["lastSampleMonotonicNs"])
        require(first_ns <= int(begin_ns) + 10_000_000 and last_ns >= int(end_ns) - 10_000_000,
                "Working-set diagnostic did not cover the complete GUI requests")
        return {"status": "MEASURED", "maximumWorkingSetBytes": int(summary["maximumWorkingSetBytes"]),
                "maximumSampleGapMilliseconds": summary.get("maximumSampleGapMilliseconds"),
                "error": None}
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        return {"status": "INVALID", "maximumWorkingSetBytes": None,
                "maximumSampleGapMilliseconds": None, "error": str(error)}


def qualify(directory):
    spec = json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))
    actions = read_lines(directory / "uia-actions.jsonl")
    events = read_lines(directory / "gui-performance.jsonl")
    verified = one(actions, lambda a: a["action"] == "verified-gui", "verified GUI identity")
    pid = int(verified["guiPid"])
    require(verified["detail"]["discoveryOnly"] is False, "Discovery process cannot qualify")
    require(same_path(verified["detail"]["image"], spec["executable"]), "UIA executable mismatch")
    require(verified["detail"]["sha256"].lower() == spec["executableSha256"].lower(), "UIA SHA mismatch")
    require(sha(spec["executable"]) == spec["executableSha256"].lower(), "Installed executable changed")
    require(Path(spec["executable"]).name.lower() == "fea.exe", "Actual product executable required")
    require(os.path.commonpath([os.path.realpath(spec["installedRoot"]), os.path.realpath(spec["executable"])]) == os.path.realpath(spec["installedRoot"]), "Installed root mismatch")
    require(not any(a["action"] == "harness-failed" for a in actions), "UIA run failed")
    baseline = one(actions, lambda a: a["action"] == "baseline-sampler-ready", "baseline sampling start")
    profile = one(actions, lambda a: a["action"] == "isolated-application-profile", "isolated application profile")
    require(profile["detail"]["launch"] is True and profile["detail"]["environment"] == "RANGE_APPLICATION_PROFILE_DIR", "Actual isolated launch required")
    require(Path(profile["detail"]["path"]).is_dir(), "Application profile missing")
    require(any(a["action"] == "picker-commit" and same_path(a["detail"]["path"], spec["sourceModel"]) for a in actions), "Source was not opened through its picker")
    require(any(a["action"] == "picker-commit" and same_path(a["detail"]["path"], spec["workspace"]) for a in actions), "Project was not opened through its picker")
    require(any(a["action"] == "picker-commit" and same_path(a["detail"]["path"], spec["csv"]) for a in actions), "CSV destination was not supplied through its picker")
    terminal_path = directory / "terminal-native-job.json"
    terminal = json.loads(terminal_path.read_text(encoding="utf-8-sig"))
    terminal_action = one(actions, lambda a: a["action"] == "terminal-native-20000", "terminal native job admission action")
    require(int(terminal_action["guiPid"]) == pid and int(terminal_action["detail"]["guiPid"]) == pid and
            int(terminal.get("guiPid", -1)) == pid, "Terminal job GUI process identity mismatch")
    for field in ("jobId", "projectId", "resultModel", "resultArtifactSha256", "acceptedSeq", "stateFingerprintSha256"):
        require(terminal_action["detail"].get(field) == terminal.get(field), "Terminal action differs from terminal record " + field)
    auto_bound = one(actions, lambda a: a["action"] == "native-result-auto-bound", "automatic registered native-result binding")
    binding = auto_bound["detail"]
    require(terminal.get("status") == "TERMINAL_NATIVE_20000", "Terminal native job admission missing")
    for field in ("jobId", "projectId", "resultModel", "resultArtifactSha256", "acceptedSeq", "stateFingerprintSha256"):
        require(binding.get(field) == terminal.get(field), "Automatic result binding differs from terminal " + field)
    require(binding.get("binding") == "product-automatic-native-job-result", "Product automatic-result binding evidence missing")
    require(binding.get("sourceFingerprint") == spec.get("sourceFingerprint"), "Automatic result binding source differs from approved source")
    require(int(terminal["acceptedSeq"]) == 20000, "Terminal native job did not admit 20000 steps")
    require(same_path(terminal["resultModel"], spec["resultModel"]), "Terminal result path differs from the qualified manifest")
    require(sha(terminal["resultModel"]) == terminal["resultArtifactSha256"].lower(), "Terminal result artifact SHA mismatch")
    for event in events:
        check_event(event, pid, spec["executable"])
    plot = one(events, lambda e: e["event"] == "canvas_painted" and e["operation"] == "plot", "actual canvas paint")
    committed = one(events, lambda e: e["event"] == "csv_committed" and e["operation"] == "csv", "atomic CSV commit")
    plot_begin = one(events, lambda e: e["event"] == "request_begin" and e["operation"] == "plot" and e["requestId"] == plot["requestId"], "automatic native-result plot request")
    native_paths = plot_begin.get("arguments", {}).get("nativePaths")
    require(isinstance(native_paths, list) and len(native_paths) == 1 and same_path(native_paths[0], terminal["resultModel"]), "Plot did not use the registered terminal native result")
    plot_binding = json.loads(base64.b64decode(plot["sourceBindingBytesBase64"], validate=True))
    require(plot.get("sourceFingerprint") == spec.get("sourceFingerprint") == binding["sourceFingerprint"], "Plot source fingerprint differs from terminal binding")
    require(plot.get("terminalStateSha256") == terminal.get("stateFingerprintSha256"), "Plot terminal state differs from admitted native job")
    require(plot_binding.get("project", {}).get("projectId") == terminal.get("projectId"), "Plot source binding project differs from terminal native job")
    requests = []
    for operation, end in (("plot", plot), ("csv", committed)):
        begin = one(events, lambda e: e["event"] == "request_begin" and e["requestId"] == end["requestId"] and e["operation"] == operation, operation + " cold request")
        require(events.index(begin) < events.index(end), "Terminal event precedes request")
        require(0 <= int(begin["elapsedNanoseconds"]) <= int(end["elapsedNanoseconds"]), "Invalid elapsed time")
        require(stamp(baseline["utc"]) <= stamp(begin["utc"]), "Baseline sampler started too late")
        check_performance_family(end, spec["family"])
        if "sourceFingerprint" in spec:
            require(end["sourceFingerprint"] == spec["sourceFingerprint"], "Dataset does not match approved source fingerprint")
        require(int(end["totalRecords"]) == 20001 and int(end["baseSeq"]) == 0 and int(end["targetSteps"]) == 20000, "Complete original 20000-step FE history required")
        require(int(end.get("acceptedSeq", end.get("lastFE", -1))) == 20000, "Terminal sequence mismatch")
        binding = base64.b64decode(end["sourceBindingBytesBase64"], validate=True)
        require(hashlib.sha256(binding).hexdigest() == end["sourceBindingSha256"], "Source binding hash mismatch")
        json.loads(binding)
        require(int(end["committedByteEnd"]) > 0, "Missing committed trace cursor")
        for field in ("sourceFingerprint", "baseModelSha256", "headerSha256", "contextSha256", "terminalRecordSha256", "terminalStateSha256"):
            require(len(end[field]) == 64 and all(c in "0123456789abcdef" for c in end[field]), "Invalid trace identity: " + field)
        requests.append(begin)
    require(plot["installedWindowQualified"] is True and plot["platform"] == "windows", "Actual installed Windows window did not qualify")
    require(all(plot[k] is True for k in ("windowVisible", "canvasVisible", "windowExposed")), "Plot is not visible/exposed")
    require(int(plot["canvasWidth"]) > 0 and int(plot["canvasHeight"]) > 0, "Empty canvas")
    require(plot["view"] == "time" and int(plot["firstFE"]) == 0 and int(plot["lastFE"]) == 20000, "Wrong displayed dataset")
    require(int(plot["firstDisplayedOrdinal"]) == 0 and int(plot["lastDisplayedOrdinal"]) == 20000, "Display thinning dropped an endpoint")
    require(2 <= int(plot["displayRecords"]) <= 20001, "Invalid display record count")
    require(committed["continuousFE"] is True and same_path(committed["destination"], spec["csv"]), "Incomplete/wrong CSV commit")
    for key in ("sourceFingerprint", "sourceBindingSha256", "configurationIdentity", "backendIdentity", "baseModelSha256", "headerSha256", "contextSha256", "terminalRecordSha256", "terminalStateSha256", "committedByteEnd"):
        require(plot[key] == committed[key], "Plot/CSV authority mismatch: " + key)
    csv_path = Path(spec["csv"])
    with csv_path.open("rb") as stream:
        prefix = stream.readline()
        require(prefix.startswith(b"# "), "CSV provenance header missing")
        metadata = json.loads(prefix[2:])
        payload = stream.read()
    require(hashlib.sha256(payload).hexdigest() == metadata["csvPayloadSha256"], "CSV payload differs from certified commit")
    check_complete_metadata(metadata, spec["family"])
    for field in ("sourceFingerprint", "configurationIdentity", "backendIdentity", "baseModelSha256", "headerSha256", "contextSha256"):
        require(metadata[field] == plot[field], "CSV metadata identity mismatch: " + field)
    rows = csv.reader(io.StringIO(payload.decode("utf-8")))
    header = next(rows)
    require(header[:5] == ["completedFE [1]", "time [s]", "controlTick [1]", "controlPhase [1]", "stateSha256"], "Unexpected native CSV columns")
    count = 0; previous = None
    for row in rows:
        require(len(row) == len(header) and int(row[0]) == count, "Missing/duplicate CSV FE row")
        value = float(row[1]); require(math.isfinite(value) and (previous is None or value > previous), "Nonfinite/nonincreasing CSV clock")
        require(len(row[4]) == 64, "Missing certified runtime hash")
        require(all(math.isfinite(float(v)) for v in row[5:]), "Nonfinite physical CSV data")
        previous = value; count += 1
    require(count == 20001, "CSV does not contain 20001 complete accepted rows")
    # Windows monotonic timestamps are system-wide; align GUI's UTC
    # observations conservatively (10 ms allowance for millisecond UTC logging).
    offset = time.monotonic() - time.time()
    gui_start_ns = int((stamp(verified["utc"]) + offset) * 1e9)
    baseline_ns = int((stamp(baseline["utc"]) + offset) * 1e9)
    begin_ns = int((min(stamp(e["utc"]) for e in requests) + offset) * 1e9)
    end_ns = int((stamp(committed["utc"]) + offset) * 1e9)
    working_set = working_set_diagnostic(directory, pid, spec["executable"], begin_ns, end_ns)
    physical_ram = check_physical_ram(directory, gui_start_ns, baseline_ns, begin_ns, end_ns)
    plot_seconds = int(plot["elapsedNanoseconds"]) / 1e9
    csv_seconds = int(committed["elapsedNanoseconds"]) / 1e9
    maximum = working_set["maximumWorkingSetBytes"]
    physical_ram_within_limit = physical_ram["maximumUsedPercent"] < PHYSICAL_RAM_LIMIT_PERCENT
    accepted_seq = int(plot.get("acceptedSeq", plot.get("lastFE", -1)))
    csv_accepted_seq = int(committed.get("acceptedSeq", committed.get("lastFE", -1)))
    require(accepted_seq == csv_accepted_seq == 20000, "Plot/CSV accepted sequence mismatch")
    return {"schema": "range.pc11-installed-gui-qualification/v1", "status": "QUALIFIED" if plot_seconds <= 5 and csv_seconds <= 30 and physical_ram_within_limit else "FAILED_LIMITS",
            "family": spec["family"], "guiPid": pid, "executable": spec["executable"], "executableSha256": spec["executableSha256"],
            "firstPlotSeconds": plot_seconds, "maximumFirstPlotSeconds": 5, "completeCsvSeconds": csv_seconds, "maximumCsvSeconds": 30,
            "records": count, "acceptedSeq": accepted_seq, "maximumWorkingSetBytes": maximum,
            "workingSetStatus": working_set["status"], "workingSetDiagnosticError": working_set["error"],
            "workingSetInformational": True,
            "maximumPhysicalRamUsedPercent": physical_ram["maximumUsedPercent"],
            "maximumPhysicalRamUsedPercentLimit": PHYSICAL_RAM_LIMIT_PERCENT,
            "physicalRamSampleCount": physical_ram["sampleCount"],
            "physicalRamSource": physical_ram["source"],
            "maximumObservedPhysicalRamSampleGapMilliseconds": physical_ram["maximumSampleGapMilliseconds"],
            "physicalRamCoverageMilliseconds": physical_ram["coveredDurationMilliseconds"],
            "samplingIntervalMilliseconds": 50,
            "maximumObservedWorkingSetSampleGapMilliseconds": working_set["maximumSampleGapMilliseconds"],
            "sourceFingerprint": plot["sourceFingerprint"], "terminalStateSha256": plot["terminalStateSha256"], "csvSha256": sha(csv_path),
            "guiAcceptanceVerified": plot_seconds <= 5 and csv_seconds <= 30 and physical_ram_within_limit, "productComplete": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = qualify(args.evidence)
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        result = {"schema": "range.pc11-installed-gui-qualification/v1", "status": "INVALID", "error": str(error), "guiAcceptanceVerified": False, "productComplete": False}
    target = args.evidence / "qualification.json"
    with target.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
    print(json.dumps(result, allow_nan=False))
    return 0 if result["status"] == "QUALIFIED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
