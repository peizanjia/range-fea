"""Seed real, independently owned WF11 workspaces for visible Qt acceptance.

No protocol replies or eligible job records are fabricated. All extension
validation uses the frozen worker and four host-owned independent cases. The
GUI must create and approve its own run preview in a new service session.
"""
import argparse
import hashlib
import importlib
import json
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime, timezone

TESTS = Path(__file__).resolve().parent
REPO_PYTHON = TESTS.parent / "python"


def _requested_package_directory(argv):
    # Read only the literal package option before imports; its value is data, never code.
    requested = str(REPO_PYTHON)
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument == "--package":
            if index + 1 >= len(argv):
                raise SystemExit("--package requires an absolute existing directory")
            requested = argv[index + 1]
            index += 2
            continue
        if argument.startswith("--package="):
            requested = argument.partition("=")[2]
        index += 1

    path = Path(requested)
    if not path.is_absolute():
        raise SystemExit("--package must be an absolute existing directory")
    try:
        path = path.resolve(strict=True)
    except OSError as error:
        raise SystemExit(f"--package does not name an existing directory: {path}") from error
    if not path.is_dir():
        raise SystemExit(f"--package does not name an existing directory: {path}")
    return path


PACKAGE_DIRECTORY = _requested_package_directory(sys.argv[1:])
sys.path.insert(0, str(PACKAGE_DIRECTORY))

import range_workflow
from range_workflow import WorkflowClient
from range_workflow.model_sources import register_snapshot_source, list_model_sources
from range_workflow import scientific_contract as contract
from range_workflow import scientific_extensions as scientific
from range_workflow.scientific_reference import reference_result


def _assert_module_from_package(module_name, module_file):
    if not module_file:
        raise RuntimeError(f"{module_name} has no import file")
    actual = Path(module_file).resolve(strict=True)
    try:
        actual.relative_to(PACKAGE_DIRECTORY)
    except ValueError as error:
        raise RuntimeError(
            f"{module_name} loaded from {actual}, outside requested package {PACKAGE_DIRECTORY}") from error


_assert_module_from_package("range_workflow", range_workflow.__file__)
_workflow_client_module = importlib.import_module(WorkflowClient.__module__)
_assert_module_from_package(WorkflowClient.__module__, _workflow_client_module.__file__)

# Test helpers may add the source tree to sys.path. The already-loaded package
# keeps its selected __path__, so all range_workflow submodules stay package-anchored.
sys.path.insert(0, str(TESTS))
sys.path.insert(0, str(REPO_PYTHON))

from test_imported_workflow import _make_catalog
from test_scientific_extensions import manifest

_selected_package_path = Path(range_workflow.__file__).resolve(strict=True).parent
if not any(Path(path).resolve(strict=True) == _selected_package_path for path in range_workflow.__path__):
    raise RuntimeError("range_workflow package search path changed after test helper imports")


def progress(phase):
    print(datetime.now(timezone.utc).isoformat() + " " + phase, file=sys.stderr, flush=True)


def verify_runtime(sdk):
    accepted = sdk.request("extension.runtime.verify", {"sdkVersion": "range.extension/v2", "force": True})
    if accepted.get("state") not in ("queued", "running") or not accepted.get("verificationId"):
        raise RuntimeError("Private runtime verification was not accepted: " + json.dumps(accepted))
    progress("private runtime verification accepted as " + accepted["state"])
    deadline = time.monotonic() + 360
    while time.monotonic() < deadline:
        result = sdk.request("extension.runtime.verification.query", {"verificationId": accepted["verificationId"]})
        if result["state"] == "ready" and result["isCurrent"] is True:
            if not result.get("runtimeHash") or not result.get("fullFileVerify", {}).get("verifiedFiles"):
                raise AssertionError("Runtime verification receipt incomplete")
            progress("private runtime verification ready " + result["runtimeHash"])
            return result
        if result["state"] not in ("queued", "running"):
            raise RuntimeError(json.dumps(result))
        time.sleep(.5)
    raise TimeoutError("Private runtime verification exceeded six-minute phase budget")


def approved(sdk, command, parameters):
    progress(command + " preview")
    preview = sdk.preview(command, parameters)
    progress(command + " approve and execute")
    job = sdk.wait(sdk.execute(preview, sdk.approve(preview))["jobId"], timeout=180)
    if job["state"] != "succeeded":
        raise RuntimeError(json.dumps(job))
    progress(command + " succeeded")
    return job


def seed_version(sdk, version, source, activate=True):
    data = scientific.golden_cases()[0]["input"]
    parameters = {"manifest": manifest(version), "source": source,
        "tests": {"cases": [{"id": "candidate-dc", "input": data,
                              "expected": reference_result(data, small_n=True)}]}}
    staged = None
    entries = [entry for entry in sdk.request("extension.list", {})["extensions"]
               if entry["extensionId"] == "spectral-test"]
    if entries:
        entry = entries[0]
        if entry.get("nextCandidateOffset") is not None:
            raise RuntimeError("Resume candidate page is incomplete; do not silently discard existing candidates")
        for summary in entry.get("candidates", []):
            if summary.get("version") != version:
                continue
            candidate = sdk.request("extension.candidate.get", {"candidateId": summary["candidateId"]})["candidate"]
            if all(candidate.get(key) == value for key, value in parameters.items()):
                staged = candidate
                progress("resuming owned candidate " + candidate["candidateId"])
                break
    if staged is None:
        staged = approved(sdk, "extension.stage", parameters)["result"]
    validated = approved(sdk, "extension.validate", {
        "candidateId": staged["candidateId"], "bundleHash": staged["bundleHash"],
    })["result"]
    if validated["validation"] != {"passed": True, "caseCount": 1, "independentCaseCount": 4}:
        raise AssertionError("Independent host validation did not pass all four cases")
    if activate:
        approved(sdk, "extension.activate", {
            "candidateId": staged["candidateId"], "bundleHash": staged["bundleHash"],
        })
    return staged


def main():
    parser = argparse.ArgumentParser()
    for name in ("root", "solver", "native", "examples", "python"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--package", type=Path, default=PACKAGE_DIRECTORY,
                        help="Absolute directory containing the selected range_workflow package")
    args = parser.parse_args()
    if args.package.resolve(strict=True) != PACKAGE_DIRECTORY:
        raise RuntimeError("The parsed --package differs from the package imported before test helpers")
    progress("package import verified: requested=" + str(PACKAGE_DIRECTORY)
             + " range_workflow=" + str(Path(range_workflow.__file__).resolve(strict=True))
             + " WorkflowClient=" + str(Path(_workflow_client_module.__file__).resolve(strict=True)))
    args.root.mkdir(parents=True, exist_ok=True)
    workspace = args.root / "workspace"
    second_workspace = args.root / "second-workspace"
    snapshot = args.root / "source.rbm"
    progress("prepare registered 20-frame source")
    if not snapshot.is_file():
        prepared = subprocess.run([
        str(args.native), "prepare", "--input", str(args.examples / "stage9_truss4.ram"),
        "--output", str(snapshot), "--steps", "20", "--output-frequency", "1", "--dt", ".005",
        ], capture_output=True, timeout=60, check=False)
        if prepared.returncode:
            raise RuntimeError(prepared.stderr.decode("utf-8", errors="replace"))
    snapshot_hash = hashlib.sha256(snapshot.read_bytes()).hexdigest()
    existing = list_model_sources(workspace)["sources"] if workspace.is_dir() else []
    registered = next((item for item in existing if item["registeredModelSha256"] == snapshot_hash), None)
    if registered is None:
        _, snapshot_ref = _make_catalog(args.root / "catalog", snapshot, snapshot_hash)
        registered = register_snapshot_source(workspace, args.root / "catalog", snapshot_ref, args.native)
    else:
        progress("resuming revalidated registered source " + registered["sourceId"])
    source = Path(scientific.__file__).with_name("scientific_example.py").read_text(encoding="utf-8")
    if contract.CONTRACT_HASH != "0708308690808434e9552c2f96e718af3c28fa15ce63fae2f6b04daf19f0565f":
        raise AssertionError("Frozen WF11 contract changed")
    options = {"window": "hann", "removeMean": True,
               "filter": {"kind": "butterworth-lowpass", "order": 1, "cutoffHz": 10.,
                          "phase": "causal", "initialCondition": "zero"}}
    with WorkflowClient(workspace, args.solver, args.native, args.examples, args.python,
                        trusted_extensions=True) as sdk:
        verify_runtime(sdk)
        first = seed_version(sdk, "1.0.0", source)
        second = seed_version(sdk, "2.0.0", source + "\n# immutable GUI acceptance version 2\n", activate=False)
        approved(sdk, "workflow.configure", {"dt": .005, "steps": 20, "targetDamping": .05,
                                               "sourceId": registered["sourceId"]})
        native_job = approved(sdk, "workflow.run", {"mode": "both", "independentReference": True})
        job_id = native_job["jobId"]
        catalog = sdk.native_result(job_id, "catalog", mode="active", limit=1)
        if catalog["frameCount"] != 20 or catalog["sourceKind"] != "native-active-fe":
            raise AssertionError("Expected 20 actual active FE frames")
        port = registered["descriptor"]["ports"]["velocity"]
        axis = "XYZ"[next(i for i, value in enumerate(port["globalDirection"]) if value)]
        sources = sdk.series_sources(job_id, mode="active")["channels"]
        selections, raw = [], []
        for quantity, unit in (("displacement", "m"), ("velocity", "m/s")):
            channel_id = f"node:{port['nodePhysicalId']}:{quantity}:{axis}"
            item = next(item for item in sources if item["selector"]["channelId"] == channel_id)
            if item["unit"] != unit:
                raise AssertionError("Native channel unit changed")
            selections.append(item["selector"])
            samples, cursor = [], None
            while True:
                page = sdk.native_result(job_id, "series", mode="active", artifactId=catalog["artifactId"],
                    channelId=channel_id, limit=7, **({"cursor": cursor} if cursor else {}))
                samples.extend(page["samples"])
                cursor = page.get("nextCursor")
                if not cursor:
                    break
            if len(samples) != 20 or not any(sample["value"] for sample in samples):
                raise AssertionError("Native raw history incomplete or identically zero")
            raw.append(samples)
        preview = sdk.preview("extension.run", {"extensionId": "spectral-test",
            "bundleHash": first["bundleHash"], "sources": selections, "options": options})
        data = preview["normalized"]["parameters"]["series"]
        for channel, samples in zip(data["channels"], raw):
            if channel["values"] != [sample["value"] for sample in samples]:
                raise AssertionError("Scientific input differs from complete actual native history")
            if data["times"] != [sample["timeS"] for sample in samples]:
                raise AssertionError("Scientific input time alignment differs")
        expected = reference_result(data, small_n=True)
    # Revalidate independently in B: never copy absolute artifact ownership.
    with WorkflowClient(second_workspace, args.solver, args.native, args.examples, args.python,
                        trusted_extensions=True) as sdk:
        verify_runtime(sdk)
        alternate = seed_version(sdk, "1.0.0", source)
        if alternate["bundleHash"] != first["bundleHash"]:
            raise AssertionError("Identical immutable bundle differs across fresh workspaces")
    fixture = {"workspace": str(workspace), "secondWorkspace": str(second_workspace),
        "sourceId": registered["sourceId"], "sourceJob": job_id, "sources": selections,
        "first": first, "second": second, "series": data, "expected": expected,
        "options": options, "python": str(args.python), "native": str(args.native),
        "solver": str(args.solver), "contractHash": contract.CONTRACT_HASH,
        "nativeCatalog": catalog, "raw": raw}
    (args.root / "fixture.json").write_text(json.dumps(fixture, allow_nan=False), encoding="utf-8")
    print(json.dumps({"workspace": str(workspace), "sourceJob": job_id,
                      "fixture": str(args.root / "fixture.json")}), flush=True)


if __name__ == "__main__":
    main()
