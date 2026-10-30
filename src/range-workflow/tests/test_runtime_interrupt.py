"""Actual host process interruption with accepted/pending runtime parameters."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_workflow import WorkflowClient
from range_workflow.model_sources import register_snapshot_source
from range_workflow.service import file_hash
from test_imported_workflow import _make_catalog
import test_recovery_runtime as recovery_tests

ARGS = None


class RuntimeInterruptTest(unittest.TestCase):
    def seed(self, base, steps):
        model = base / "source.rbm"
        prepared = subprocess.run([str(ARGS.native), "prepare", "--input",
            str(ARGS.examples / "stage9_truss4.ram"), "--output", str(model), "--steps", str(steps),
            "--output-frequency", "1", "--dt", ".005"], capture_output=True, timeout=60)
        self.assertEqual(prepared.returncode, 0, prepared.stderr.decode(errors="replace"))
        _, ref = _make_catalog(base / "catalog", model, file_hash(model))
        source = register_snapshot_source(base / "workspace", base / "catalog", ref, ARGS.native)
        with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
            preview = sdk.preview("workflow.configure", {"dt": .005, "steps": steps,
                "targetDamping": .1, "sourceId": source["sourceId"]})
            configured = sdk.execute(preview, sdk.approve(preview))
            self.assertEqual(sdk.wait(configured["jobId"])["state"], "succeeded")

    def paused(self, client, job_id):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            job = client.request("jobs.query", {"jobId": job_id})["job"]
            self.assertNotIn(job["state"], {"failed", "cancelled", "succeeded"}, job.get("error"))
            state = client.request("runtime.parameters.query", {"jobId": job_id})
            if state.get("execution", {}).get("state") == "paused":
                attempts = state["runtimeControl"]["attempts"]
                return next(attempt["acceptedState"]["parameters"] for attempt in attempts.values()
                            if attempt["status"] == "ready")
            time.sleep(.025)
        self.fail("Native accepted boundary did not pause")

    def proposal(self, client, job_id, before):
        record = client.request("runtime.parameters.preview", {"jobId": job_id,
            "velocityGainNsm": before["velocityGainNsm"] * 1.2, "actuatorLimits": before["actuatorLimits"]})
        approval = client.request("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": record["proposalId"]})
        client.request("runtime.parameters.submit", {"jobId": job_id, "proposalId": record["proposalId"],
                                                      "approvalToken": approval["approvalToken"]})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            queued = client.request("runtime.parameters.query", {"jobId": job_id, "proposalId": record["proposalId"]})
            if queued["status"] == "queued":
                return record
            time.sleep(.025)
        self.fail("Native proposal was not observed queued")

    def recover(self, workspace, job_id):
        with WorkflowClient(workspace, ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
            old = sdk.request("jobs.query", {"jobId": job_id})["job"]
            self.assertEqual(old["state"], "interrupted")
            qualification = sdk.recovery(job_id)
            self.assertEqual(qualification["eligibility"], "recoverable", qualification)
            preview = sdk.preview("workflow.recover", {"jobId": job_id,
                "qualificationSha256": qualification["qualificationSha256"]})
            launched = sdk.execute(preview, sdk.approve(preview))
            self.assertEqual(launched["jobId"], job_id)
            completed = sdk.wait(job_id, timeout=120)
            self.assertEqual(completed["state"], "succeeded", completed.get("error"))
            self.assertLessEqual(completed["result"]["active"]["maxDisplacementErrorM"], 1e-8)
            self.assertLessEqual(completed["result"]["active"]["maxVelocityErrorMPerSecond"], 1e-7)
            return completed

    def test_accepted_change_survives_real_checkpoint_process_exit(self):
        if not ARGS or ARGS.solver is None:
            self.skipTest("Explicit native paths required")
        with tempfile.TemporaryDirectory(prefix="range-runtime-accepted-crash-") as temporary:
            base = Path(temporary); self.seed(base, 80)
            with recovery_tests.InjectedRuntimeClient(base / "workspace",
                    {"kind": "checkpoint-watch", "variant": "active", "acceptedSeq": 10}) as client:
                preview = client.preview("workflow.run", {"mode": "active", "independentReference": True,
                    "runtimeControl": True, "pauseAtAcceptedSeq": 2})
                job_id = client.execute(preview, client.approve(preview))["jobId"]
                before = self.paused(client, job_id)
                proposal = self.proposal(client, job_id, before)
                client.request("runtime.execution.resume", {"jobId": job_id})
                self.assertEqual(client.wait_for_exit(job_id, timeout=90), 72)
            completed = self.recover(base / "workspace", job_id)
            state = completed["result"]["runtimeParameters"]
            self.assertEqual(state["parameters"]["revision"], "1")
            self.assertEqual(state["history"][0]["proposalId"], proposal["proposalId"])
            self.assertEqual(state["history"][0]["appliedSeq"], "3")
            self.assertEqual(state["parameters"]["parameterSha256"], proposal["parameters"]["parameterSha256"])

    def test_queued_only_change_is_not_replayed_after_process_kill(self):
        if not ARGS or ARGS.solver is None:
            self.skipTest("Explicit native paths required")
        with tempfile.TemporaryDirectory(prefix="range-runtime-pending-crash-") as temporary:
            base = Path(temporary); self.seed(base, 24)
            with recovery_tests.InjectedRuntimeClient(base / "workspace") as client:
                preview = client.preview("workflow.run", {"mode": "active", "independentReference": True,
                    "runtimeControl": True, "pauseAtAcceptedSeq": 11})
                job_id = client.execute(preview, client.approve(preview))["jobId"]
                before = self.paused(client, job_id)
                proposal = self.proposal(client, job_id, before)
                client.process.kill(); client.process.wait(timeout=10)
            completed = self.recover(base / "workspace", job_id)
            state = completed["result"]["runtimeParameters"]
            self.assertEqual(state["parameters"]["revision"], "0")
            self.assertEqual(state["history"], [])
            self.assertEqual(state["parameters"]["parameterSha256"], before["parameterSha256"])
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
                old = sdk.runtime_parameters(job_id, proposal["proposalId"])
                self.assertEqual(old["status"], "unknown")
                self.assertNotIn("appliedSeq", old.get("receipt", {}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    for name in ("solver", "native", "examples", "python"):
        parser.add_argument("--" + name, type=Path, default=Path(sys.executable) if name == "python" else None)
    ARGS, remaining = parser.parse_known_args()
    recovery_tests.ARGS = ARGS
    unittest.main(argv=[sys.argv[0], *remaining])
