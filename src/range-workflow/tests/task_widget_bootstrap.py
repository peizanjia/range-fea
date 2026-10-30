"""Prepare a real registered source for the Qt task widget acceptance test.

This helper configures only the baseline; all task proposals and approvals in
the acceptance story must subsequently come from visible Qt controls.
"""
import sys
sys.dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

_TESTS_DIR = Path(__file__).resolve().parent
sys.path[:0] = [str(_TESTS_DIR.parent / "python"), str(_TESTS_DIR)]

from test_imported_workflow import _make_catalog
from range_workflow.client import WorkflowClient
from range_workflow.model_sources import register_snapshot_source


def main():
    parser = argparse.ArgumentParser()
    for name in ("root", "solver", "native", "examples", "python"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    snapshot = args.root / "source.rbm"
    prepared = subprocess.run([
        str(args.native), "prepare", "--input", str(args.examples / "stage9_truss4.ram"),
        "--output", str(snapshot), "--steps", "20", "--output-frequency", "1", "--dt", "0.005",
    ], capture_output=True, timeout=60, check=False)
    if prepared.returncode:
        raise RuntimeError(prepared.stderr.decode("utf-8", errors="replace"))
    _, reference = _make_catalog(args.root / "catalog", snapshot,
                                 hashlib.sha256(snapshot.read_bytes()).hexdigest())
    source = register_snapshot_source(args.root / "workspace", args.root / "catalog", reference, args.native)
    with WorkflowClient(args.root / "workspace", args.solver, args.native, args.examples, args.python) as sdk:
        preview = sdk.preview("workflow.configure", {
            "dt": .005, "steps": 20, "targetDamping": .05, "sourceId": source["sourceId"],
        })
        accepted = sdk.execute(preview, sdk.approve(preview))
        job = sdk.wait(accepted["jobId"], timeout=60)
        if job["state"] != "succeeded":
            raise RuntimeError(json.dumps(job))
    (args.root / "fixture.json").write_text(json.dumps({
        "sourceId": source["sourceId"], "modelSha256": source["registeredModelSha256"],
    }), encoding="utf-8")


if __name__ == "__main__":
    main()
