"""Actual native controller failure rolls back a candidate parameter step."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import WorkflowClient
from range_workflow.model_sources import register_snapshot_source
from range_workflow.service import file_hash
from test_imported_workflow import _make_catalog

ARGS = None


class RuntimeFailureTest(unittest.TestCase):
    def test_actual_candidate_compute_failure_preserves_accepted_model_and_parameters(self):
        if not ARGS or ARGS.solver is None:
            self.skipTest("Explicit native paths required")
        with tempfile.TemporaryDirectory(prefix="range-runtime-compute-failure-") as temporary:
            base = Path(temporary)
            examples = base / "examples"
            shutil.copytree(ARGS.examples, examples, ignore=shutil.ignore_patterns("__pycache__"))
            controller_path = examples / "stage9_velocity_controller.py"
            source_code = controller_path.read_text(encoding="utf-8")
            needle = "        velocity = observed[self.sensor_id]\n"
            self.assertEqual(source_code.count(needle), 1)
            source_code = source_code.replace(needle, needle +
                "        if request.get('runtimeParameters', {}).get('parameters', {}).get('revision', '0') != '0':\n"
                "            raise ControllerError('Injected candidate compute failure')\n")
            controller_path.write_text(source_code, encoding="utf-8")
            snapshot = base / "source.rbm"
            prepared = subprocess.run([str(ARGS.native), "prepare", "--input", str(examples / "stage9_truss4.ram"),
                "--output", str(snapshot), "--steps", "20", "--output-frequency", "1", "--dt", ".005"],
                capture_output=True, timeout=60)
            self.assertEqual(prepared.returncode, 0, prepared.stderr.decode(errors="replace"))
            _, ref = _make_catalog(base / "catalog", snapshot, file_hash(snapshot))
            registered = register_snapshot_source(base / "workspace", base / "catalog", ref, ARGS.native)
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, examples, ARGS.python) as sdk:
                configure = sdk.preview("workflow.configure", {"dt": .005, "steps": 20,
                    "targetDamping": .1, "sourceId": registered["sourceId"]})
                launched = sdk.execute(configure, sdk.approve(configure))
                self.assertEqual(sdk.wait(launched["jobId"])["state"], "succeeded")
                run = sdk.preview("workflow.run", {"mode": "active", "independentReference": True,
                    "runtimeControl": True, "pauseAtAcceptedSeq": 2})
                job_id = sdk.execute(run, sdk.approve(run))["jobId"]
                deadline = time.monotonic() + 60
                before = None
                while time.monotonic() < deadline:
                    current = sdk.runtime_parameters(job_id)
                    if current.get("execution", {}).get("state") == "paused":
                        before = next(attempt["acceptedState"]["parameters"] for attempt in
                            current["runtimeControl"]["attempts"].values() if attempt["status"] == "ready")
                        break
                    time.sleep(.025)
                self.assertIsNotNone(before)
                active = base / "workspace" / "jobs" / job_id / "active"
                outputs = list(active.glob("active-00002.rbm"))
                if not outputs:
                    outputs = [path for path in active.glob("*.rbm") if "00002" in path.name]
                self.assertEqual(len(outputs), 1, [p.name for p in active.iterdir()])
                accepted_path = outputs[0]
                accepted_sha = file_hash(accepted_path)
                proposal = sdk.preview_runtime_parameters(job_id, before["velocityGainNsm"] * 1.2,
                                                          before["actuatorLimits"])
                approval = sdk.request("host.runtime.parameters.approve", {"jobId": job_id,
                    "proposalId": proposal["proposalId"]})
                sdk.submit_runtime_parameters(job_id, proposal["proposalId"], approval["approvalToken"])
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    queued = sdk.runtime_parameters(job_id, proposal["proposalId"])
                    if queued["status"] == "queued":
                        break
                    time.sleep(.025)
                self.assertEqual(queued["status"], "queued")
                sdk.runtime_execution(job_id, "resume")
                failed = sdk.wait(job_id, timeout=60)
                self.assertIn(failed["state"], {"failed", "cancelled"})
                receipt = sdk.runtime_parameters(job_id, proposal["proposalId"])
                self.assertEqual(receipt["status"], "rejected")
                self.assertEqual(receipt["receipt"]["reason"], "rolled-back")
                state = sdk.runtime_parameters(job_id)["runtimeControl"]["attempts"][job_id]["acceptedState"]
                self.assertEqual(state["parameters"], before)
                self.assertEqual(state["history"], [])
                self.assertEqual(file_hash(accepted_path), accepted_sha)
                journal = (active / "native-result-frames.jsonl")
                if not journal.exists():
                    journals = [path for path in active.glob("*.jsonl") if "result" in path.name]
                    self.assertEqual(len(journals), 1)
                    journal = journals[0]
                frames = [json.loads(line) for line in journal.read_bytes().splitlines()]
                self.assertEqual([frame["recordNo"] for frame in frames], [1, 2])
                log = (base / "workspace" / "jobs" / job_id / "solve-active.log").read_text(errors="replace")
                self.assertIn("ControlComputeFailed", log)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    for name in ("solver", "native", "examples", "python"):
        parser.add_argument("--" + name, type=Path, default=Path(sys.executable) if name == "python" else None)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
