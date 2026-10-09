"""Frozen WF10 real-FE scale characterization; does not change acceptance limits."""
import argparse
import base64
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import threading
import time


def tree_private_bytes(root_pid):
    class Entry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("name", wintypes.WCHAR * 260)]
    class Memory(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ("peakWorking", "working", "peakPaged", "paged", "peakNonpaged",
                                               "nonpaged", "pagefile", "peakPagefile", "private")]
    kernel, psapi = ctypes.WinDLL("kernel32", use_last_error=True), ctypes.WinDLL("psapi", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == wintypes.HANDLE(-1).value:
        raise OSError("Cannot enumerate benchmark process tree")
    parents = {}
    entry = Entry(); entry.dwSize = ctypes.sizeof(entry)
    try:
        valid = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while valid:
            parents[int(entry.pid)] = int(entry.parent)
            valid = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    selected = {root_pid}
    while True:
        added = {pid for pid, parent in parents.items() if parent in selected} - selected
        if not added:
            break
        selected |= added
    total = 0
    for pid in selected:
        handle = kernel.OpenProcess(0x1000 | 0x10, False, pid)
        if not handle:
            if pid == root_pid:
                raise OSError("Cannot read benchmark parent process memory")
            continue  # A child can exit between enumeration and opening.
        try:
            memory = Memory(); memory.cb = ctypes.sizeof(memory)
            if psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
                total += memory.private
        finally:
            kernel.CloseHandle(handle)
    return total


def main():
    parser = argparse.ArgumentParser()
    for name in ("package", "solver", "native", "examples", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--steps", type=int, default=5000)
    args = parser.parse_args()
    if os.name != "nt" or args.steps != 5000:
        parser.error("The frozen characterization requires Windows and exactly 5000 steps")
    sys.path.insert(0, str(args.package))
    from range_workflow import WorkflowClient
    from range_workflow.model_sources import register_snapshot_source
    from range_workflow.service import file_hash
    # Fixture authoring helper does not grant runtime authority.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src/range-workflow/tests"))
    from test_imported_workflow import _make_catalog
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    workspace = output / "workspace"
    report = {"schema": "range.wf10-performance/v1", "scope": "backend-real-FE-characterization",
              "environment": {"os": platform.platform(), "machine": platform.machine(), "python": sys.version,
                              "threads": os.environ.get("OMP_NUM_THREADS"),
                              "solverSha256": file_hash(args.solver), "nativeSha256": file_hash(args.native),
                              "brokerSha256": file_hash(args.native.with_name("range-native-results.exe")),
                              "package": str(args.package.resolve())},
              "scale": {"jobs": 2, "activeSteps": 5000, "passiveSteps": 5000, "nativeRawPoints": 20000},
              "gates": {"firstFourTraceSeconds": 5, "readValidateExportSeconds": 30, "privateDeltaBytes": 512 * 1024 * 1024},
              "jobs": [], "timings": {}}
    snapshot = output / "source.rbm"
    prepared = subprocess.run([str(args.native), "prepare", "--input", str(args.examples / "stage9_truss4.ram"),
        "--output", str(snapshot), "--steps", "5000", "--output-frequency", "1", "--dt", ".005"],
        capture_output=True, timeout=60)
    if prepared.returncode:
        raise RuntimeError(prepared.stderr.decode(errors="replace"))
    _, ref = _make_catalog(output / "catalog", snapshot, file_hash(snapshot))
    source = register_snapshot_source(workspace, output / "catalog", ref, args.native)
    with WorkflowClient(workspace, args.solver, args.native, args.examples, args.python) as sdk:
        for damping in (.05, .1):
            config = sdk.preview("workflow.configure", {"dt": .005, "steps": 5000,
                "targetDamping": damping, "sourceId": source["sourceId"]})
            configured = sdk.execute(config, sdk.approve(config)); done = sdk.wait(configured["jobId"], timeout=60)
            if done["state"] != "succeeded":
                raise RuntimeError(done.get("error"))
            preview = sdk.preview("workflow.run", {"mode": "both", "independentReference": True})
            launched = sdk.execute(preview, sdk.approve(preview)); started = time.perf_counter()
            done = sdk.wait(launched["jobId"], timeout=600)
            if done["state"] != "succeeded":
                raise RuntimeError(done.get("error"))
            report["jobs"].append(done["jobId"]); report["timings"]["solve-" + done["jobId"]] = time.perf_counter() - started
            (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        selected, reference = [], None
        for job in report["jobs"]:
            for mode in ("active", "passive"):
                catalog = sdk.series_sources(job, mode)
                channel = next(c for c in catalog["channels"] if c["selector"]["channelId"] ==
                    f"node:{source['descriptor']['ports']['velocity']['nodePhysicalId']}:velocity:X")
                selected.append(channel["selector"])
                reference = reference or channel.get("reference")
        baseline = tree_private_bytes(sdk.process.pid); peak = [baseline]; stop = threading.Event()
        def monitor():
            while not stop.wait(.05):
                peak[0] = max(peak[0], tree_private_bytes(sdk.process.pid))
        sampler = threading.Thread(target=monitor, daemon=True); sampler.start()
        try:
            started = time.perf_counter(); plot = sdk.series_plot(selected, limit=1000)
            report["timings"]["firstFourTraceSeconds"] = time.perf_counter() - started
            if len(plot["traces"]) != 4 or any(t["rawSampleCount"] != 5000 or t["displaySampleCount"] > 1000 for t in plot["traces"]):
                raise AssertionError("Frozen raw/display scale differs")
            if reference is None:
                raise AssertionError("Matching independent reference is unavailable")
            started = time.perf_counter(); error = sdk.series_plot([selected[0], reference], view="error", limit=1000)
            report["timings"]["independentReferenceSeconds"] = time.perf_counter() - started
            report["referenceMaxDisplayError"] = max(abs(p["y"]) for p in error["traces"][0]["points"])
            started = time.perf_counter(); exported = sdk.series_export(plot["export"]["sources"])
            checksum = hashlib.sha256(); offset = 0
            with (output / "complete-raw.csv").open("wb") as stream:
                while offset < exported["bytes"]:
                    chunk = sdk.series_export_chunk(exported["exportId"], offset)
                    data = base64.b64decode(chunk["data"], validate=True)
                    if chunk["offset"] != offset or len(data) != chunk["length"]:
                        raise AssertionError("Export chunk identity differs")
                    stream.write(data); checksum.update(data); offset += len(data)
            report["timings"]["readValidateExportSeconds"] = time.perf_counter() - started
            if exported["count"] != 20000 or checksum.hexdigest() != exported["sha256"]:
                raise AssertionError("Complete raw export identity differs")
            report["export"] = exported
        finally:
            stop.set(); sampler.join(2)
            report["privateBytes"] = {"baselineTree": baseline, "peakTree": peak[0], "delta": peak[0] - baseline}
            report["passedBackendGates"] = all((report["timings"].get("firstFourTraceSeconds", float("inf")) <= 5,
                report["timings"].get("readValidateExportSeconds", float("inf")) <= 30, peak[0] - baseline <= 512 * 1024 * 1024))
            (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"passedBackendGates": report["passedBackendGates"], "timings": report["timings"], "privateBytes": report["privateBytes"]}))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Preserve failures before the first job completes as well as timing
        # failures. Never manufacture measurements for unfinished phases.
        if "--output" in sys.argv:
            failure_directory = Path(sys.argv[sys.argv.index("--output") + 1]).resolve()
            failure_directory.mkdir(parents=True, exist_ok=True)
            (failure_directory / "failure.json").write_text(json.dumps({
                "schema": "range.wf10-performance-failure/v1",
                "passedBackendGates": False, "errorType": type(error).__name__,
                "error": str(error), "argv": sys.argv[1:],
            }, indent=2), encoding="utf-8")
        raise
