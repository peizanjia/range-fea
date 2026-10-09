"""Run real native control on the snapshot captured after the WF14 GUI beam edit."""
import argparse
import hashlib
import importlib
import json
import math
from pathlib import Path
import subprocess
import sys


def requested_package(argv):
    # Select the package before importing it, including with embedded Python's _pth.
    value = None
    for index, argument in enumerate(argv):
        if argument == "--package" and index + 1 < len(argv):
            value = argv[index + 1]
        elif argument.startswith("--package="):
            value = argument.partition("=")[2]
    if value is None or not Path(value).is_absolute():
        raise SystemExit("--package requires an absolute existing package directory")
    path = Path(value).resolve(strict=True)
    if not path.is_dir():
        raise SystemExit("--package requires an existing directory")
    return path


PACKAGE = requested_package(sys.argv[1:])
sys.path.insert(0, str(PACKAGE))
import range_workflow
from range_workflow import WorkflowClient
from range_workflow.model_sources import register_snapshot_source
from range_workflow.project_catalog import resolve_project_snapshot

for module in (range_workflow, importlib.import_module(WorkflowClient.__module__),
               importlib.import_module(register_snapshot_source.__module__),
               importlib.import_module(resolve_project_snapshot.__module__)):
    Path(module.__file__).resolve(strict=True).relative_to(PACKAGE)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def approved(sdk, command, parameters):
    print(command, file=sys.stderr, flush=True)
    preview = sdk.preview(command, parameters)
    job = sdk.wait(sdk.execute(preview, sdk.approve(preview))["jobId"], timeout=180)
    require(job["state"] == "succeeded", json.dumps(job))
    return job


def main():
    parser = argparse.ArgumentParser()
    for name in ("package", "python", "solver", "native", "examples", "catalog", "input", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    require(args.package.resolve(strict=True) == PACKAGE, "Package selection changed")
    gui = json.loads(args.input.read_bytes())
    snapshot = gui["snapshot"]
    resolved = resolve_project_snapshot(args.catalog, snapshot["persistentDocumentId"])
    for field in ("projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef"):
        require(resolved[field] == snapshot[field], "GUI snapshot identity differs: " + field)
    original_path = Path(gui["originalFixture"])
    original_sha = sha(original_path)
    native = subprocess.run([str(args.native), "describe", "--input", str(original_path),
                             "--expected-sha", original_sha], capture_output=True, timeout=30)
    require(native.returncode == 0, native.stderr.decode(errors="replace"))
    original = json.loads(native.stdout)
    workspace = args.output.parent / "control-workspace"
    reference = {field: resolved[field] for field in
                 ("projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef")}
    registered = register_snapshot_source(workspace, args.catalog, reference, args.native)
    descriptor = registered["descriptor"]
    require(descriptor["status"] == "supported" and descriptor["modelKind"] == "cantilever-beam",
            "Edited beam must pass actual native preflight")
    require(registered["registeredModelSha256"] == snapshot["modelSha256"], "Registered hash differs")
    for field in ("elements", "pointElements", "supports", "freeDofs", "ports", "dynamics", "originalControl"):
        require(descriptor[field] == original[field], "Geometry edit changed global configuration: " + field)
    require(len(descriptor["nodes"]) == len(original["nodes"]) == 2, "Unexpected beam nodes")
    for before, after in zip(original["nodes"], descriptor["nodes"]):
        expected = dict(before)
        if before["physicalId"] == gui["geometryDelta"]["physicalNodeId"]:
            require(before["coordinates"] == gui["geometryDelta"]["before"], "Original coordinates differ")
            expected["coordinates"] = gui["geometryDelta"]["after"]
        require(after == expected, "Registered geometry or initial state differs from GUI edit")
    port = descriptor["ports"]["velocity"]
    require(port["nodePhysicalId"] == 117 and port["globalDirection"] == [0, 1, 0],
            "Physical monitor and global direction changed")
    with WorkflowClient(workspace, args.solver, args.native, args.examples, args.python) as sdk:
        configured = approved(sdk, "workflow.configure", {"dt": .0001, "steps": 20,
            "targetDamping": .1, "model": descriptor["modelKind"], "sourceId": registered["sourceId"]})
        job = approved(sdk, "workflow.run", {"mode": "both", "independentReference": True})
        result = job["result"]
        require(result["totalSteps"] == 20 and result["independentReference"] is True,
                "Incomplete independent control run")
        require(result["modelBinding"]["sourceId"] == registered["sourceId"] and
                result["modelBinding"]["registeredModelSha256"] == snapshot["modelSha256"],
                "Control run was not bound to the edited GUI snapshot")
        replay = json.loads(Path(result["referencePath"]).read_bytes())
        require(any(replay["timeOutputs"]["requestedForceN"]), "Actual active controller requested no force")
        histories = {}
        for mode in ("active", "passive"):
            state = result[mode]
            require(state["acceptedSeq"] == 20, "Incomplete accepted native trajectory")
            for field, limit in (("maxDisplacementErrorM", 1e-8), ("maxVelocityErrorMPerSecond", 1e-7),
                                 ("maxRotationErrorRad", 1e-8), ("maxAngularVelocityErrorRadPerSecond", 1e-7)):
                require(math.isfinite(state[field]) and state[field] <= limit, field + " exceeds frozen tolerance")
            catalog = sdk.native_result(job["jobId"], "catalog", mode=mode, limit=1)
            require(catalog["frameCount"] == 20 and catalog["sourceKind"] == "native-" + mode + "-fe",
                    "Expected twenty actual native frames")
            require(catalog["sourceSnapshotRef"]["modelSha256"] == snapshot["modelSha256"],
                    "Native history source differs from GUI snapshot")
            channels = {}
            for quantity in ("displacement", "velocity"):
                channel = f"node:117:{quantity}:Y"
                samples, cursor = [], None
                while True:
                    page = sdk.native_result(job["jobId"], "series", mode=mode,
                        artifactId=catalog["artifactId"], channelId=channel, limit=7,
                        **({"cursor": cursor} if cursor else {}))
                    require(page["nodePhysicalId"] == 117, "Native monitor identity changed")
                    samples.extend(page["samples"])
                    next_cursor = page.get("nextCursor")
                    if not next_cursor:
                        break
                    require(next_cursor != cursor, "Native cursor did not advance")
                    cursor = next_cursor
                require(len(samples) == 20 and all(math.isfinite(s["value"]) for s in samples),
                        "Native history incomplete or nonfinite")
                require(all(abs(s["timeS"] - (i + 1) * .0001) <= 1e-12
                            for i, s in enumerate(samples)), "Native time grid changed")
                channels[quantity] = samples
            if mode == "active":
                for sample in channels["velocity"]:
                    index = round(sample["timeS"] / .0001)
                    require(abs(sample["value"] - replay["timeOutputs"]["fullVelocityMPerSecond"][index]) <= 1e-7,
                            "Actual native velocity history disagrees with independent reference")
            histories[mode] = {"catalog": catalog, "channels": channels, "acceptedState": state}
    require(sha(resolved["path"]) == snapshot["modelSha256"], "Frozen GUI snapshot changed during solve")
    evidence = {"gui": gui, "workspace": str(workspace), "packageDirectory": str(PACKAGE),
        "packageModule": str(Path(range_workflow.__file__).resolve()),
        "python": str(args.python), "solver": str(args.solver), "native": str(args.native),
        "originalFixtureSha256": original_sha, "registeredSource": registered,
        "configurationJob": configured["jobId"], "controlJob": job["jobId"],
        "result": result, "histories": histories,
        "referenceSha256": sha(result["referencePath"]),
        "sourceReferenceSha256": sha(result["sourceReferencePath"]),
        "scope": "Same GUI session: edited POINT/BEAM geometry with native control, then independent closed TRI shell mesh document"}
    args.output.write_text(json.dumps(evidence, allow_nan=False, indent=2), encoding="utf-8")
    print(json.dumps({"evidence": str(args.output), "controlJob": job["jobId"]}), flush=True)


if __name__ == "__main__":
    main()
