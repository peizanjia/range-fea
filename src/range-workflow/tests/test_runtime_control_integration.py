"""Actual registered-source native accepted-boundary hot gain story."""
import argparse
import json
from pathlib import Path
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


class RuntimeControlIntegration(unittest.TestCase):
    def test_actual_pause_queue_gain_limit_switch_and_solver_receipt(self):
        if ARGS is None or ARGS.solver is None:
            self.skipTest("Explicit native runtime paths are required")
        with tempfile.TemporaryDirectory(prefix="range-runtime-control-") as temporary:
            base = Path(temporary)
            snapshot = base / "source.rbm"
            prepared = subprocess.run([str(ARGS.native), "prepare", "--input",
                str(ARGS.examples / "stage9_truss4.ram"), "--output", str(snapshot),
                "--steps", "40", "--output-frequency", "1", "--dt", ".005"],
                capture_output=True, timeout=60)
            self.assertEqual(prepared.returncode, 0, prepared.stderr.decode(errors="replace"))
            _, ref = _make_catalog(base / "catalog", snapshot, file_hash(snapshot))
            source = register_snapshot_source(base / "workspace", base / "catalog", ref, ARGS.native)
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
                preview = sdk.preview("workflow.configure", {"dt": .005, "steps": 40,
                    "targetDamping": .1, "model": "four-rod", "sourceId": source["sourceId"]})
                configured = sdk.execute(preview, sdk.approve(preview))
                self.assertEqual(sdk.wait(configured["jobId"])["state"], "succeeded")
                preview = sdk.preview("workflow.run", {"mode": "active", "independentReference": True,
                    "runtimeControl": True, "pauseAtAcceptedSeq": 2})
                launched = sdk.execute(preview, sdk.approve(preview))
                job_id = launched["jobId"]
                deadline = time.monotonic() + 90
                ready, job, execution = None, None, None
                while time.monotonic() < deadline:
                    job = sdk.request("jobs.query", {"jobId": job_id})["job"]
                    if job["state"] in {"failed", "cancelled", "interrupted", "succeeded"}:
                        logs = list((base / "workspace").rglob("solve-active.log"))
                        detail = logs[0].read_text(errors="replace")[-6000:] if logs else "No native log"
                        self.fail(f"Native job ended before the initial pause: {job.get('error')}\n{detail}")
                    runtime = sdk.runtime_parameters(job_id)
                    control = runtime.get("runtimeControl")
                    execution_ref = job.get("runtimeExecution")
                    if control and execution_ref:
                        attempt = control["attempts"][execution_ref["runId"]]
                        path = Path(execution_ref["path"])
                        if attempt["status"] == "ready" and path.is_file():
                            execution = json.loads(path.read_bytes())
                            if execution.get("state") == "paused":
                                ready = attempt
                                break
                    time.sleep(.05)
                self.assertIsNotNone(ready, "Native pause and runtime handshake were not observed")
                self.assertEqual(execution["acceptedSeq"], 2)
                before = ready["acceptedState"]["parameters"]
                cancelled = sdk.preview_runtime_parameters(job_id, before["velocityGainNsm"] * 1.1,
                                                           before["actuatorLimits"])
                cancelled_approval = sdk.request("host.runtime.parameters.approve", {
                    "jobId": job_id, "proposalId": cancelled["proposalId"]})
                sdk.submit_runtime_parameters(job_id, cancelled["proposalId"], cancelled_approval["approvalToken"])
                sdk.request("runtime.parameters.cancel", {"jobId": job_id, "proposalId": cancelled["proposalId"]})
                cancel_deadline = time.monotonic() + 10
                while time.monotonic() < cancel_deadline:
                    cancelled_record = sdk.runtime_parameters(job_id, cancelled["proposalId"])
                    if cancelled_record["status"] == "rejected":
                        break
                    time.sleep(.05)
                self.assertEqual(cancelled_record["status"], "rejected")
                self.assertEqual(cancelled_record["receipt"]["reason"], "cancelled")
                self.assertEqual(sdk.request("jobs.query", {"jobId": job_id})["job"]["state"], "running")
                self.assertEqual(sdk.runtime_parameters(job_id)["execution"]["state"], "paused")
                limits = [{**before["actuatorLimits"][0], "minN": -1e4,
                           "maxN": 1e4, "rateNPerS": 1000.0}]
                proposal = sdk.preview_runtime_parameters(job_id, before["velocityGainNsm"] * 1.2, limits)
                approval = sdk.request("host.runtime.parameters.approve", {
                    "jobId": job_id, "proposalId": proposal["proposalId"]})
                submitted = sdk.submit_runtime_parameters(job_id, proposal["proposalId"], approval["approvalToken"])
                self.assertEqual(submitted["status"], "intent")
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    queued = sdk.runtime_parameters(job_id, proposal["proposalId"])
                    if queued["status"] == "queued":
                        break
                    time.sleep(.05)
                self.assertEqual(queued["status"], "queued")
                self.assertNotIn("appliedSeq", queued.get("receipt", {}))
                self.assertEqual(json.loads(Path(job["runtimeExecution"]["path"]).read_bytes())["acceptedSeq"], 2)
                sdk.runtime_execution(job_id, "resume")
                completed = sdk.wait(job_id, timeout=90)
                self.assertEqual(completed["state"], "succeeded", completed.get("error"))
                accepted = sdk.runtime_parameters(job_id, proposal["proposalId"])
                self.assertEqual(accepted["status"], "accepted")
                self.assertEqual(accepted["receipt"]["appliedSeq"], "3")
                self.assertAlmostEqual(accepted["receipt"]["acceptedTimeS"], .015)
                self.assertEqual(accepted["receipt"]["parameterSha256"], proposal["parameters"]["parameterSha256"])
                self.assertLessEqual(completed["result"]["active"]["maxDisplacementErrorM"], 1e-8)
                self.assertLessEqual(completed["result"]["active"]["maxVelocityErrorMPerSecond"], 1e-7)
                self.assertEqual(completed["result"]["runtimeParameters"]["history"][0]["appliedSeq"], "3")
                last_job_id = job_id
                for extra in (5, 3):
                    resume_preview = sdk.preview("workflow.resume", {"jobId": last_job_id, "additionalSteps": extra})
                    resumed_submission = sdk.execute(resume_preview, sdk.approve(resume_preview))
                    resumed = sdk.wait(resumed_submission["jobId"], timeout=90)
                    self.assertEqual(resumed["state"], "succeeded", resumed.get("error"))
                    self.assertEqual(resumed["result"]["runtimeParameters"]["parameters"]["revision"], "1")
                    self.assertEqual(resumed["result"]["runtimeParameters"]["history"][0]["appliedSeq"], "3")
                    self.assertIn(proposal["proposalId"], resumed["result"]["runtimeProposalRecords"])
                    self.assertLessEqual(resumed["result"]["active"]["maxDisplacementErrorM"], 1e-8)
                    self.assertLessEqual(resumed["result"]["active"]["maxVelocityErrorMPerSecond"], 1e-7)
                    last_job_id = resumed["jobId"]
            # Reopening validates accepted proof and never recreates live authority.
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened:
                saved = reopened.runtime_parameters(job_id, proposal["proposalId"])
                self.assertEqual(saved["status"], "accepted")
                self.assertEqual(saved["receipt"]["appliedSeq"], "3")
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened_again:
                saved_again = reopened_again.runtime_parameters(job_id, proposal["proposalId"])
                self.assertEqual(saved_again["status"], "accepted")
                cancelled_again = reopened_again.runtime_parameters(job_id, cancelled["proposalId"])
                self.assertEqual(cancelled_again["status"], "rejected")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--solver", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--examples", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
