"""Authoritative final-interval receipt must survive native process shutdown."""
import argparse
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_workflow import WorkflowClient
import test_runtime_interrupt as interrupt_tests

ARGS = None


class FinalReceiptTest(unittest.TestCase):
    def test_actual_final_interval_receipt_is_durable_after_native_exit(self):
        if not ARGS or ARGS.solver is None:
            self.skipTest("Explicit native paths required")
        with tempfile.TemporaryDirectory(prefix="range-runtime-final-receipt-") as temporary:
            base = Path(temporary)
            helper = interrupt_tests.RuntimeInterruptTest()
            helper.seed(base, 20)
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
                preview = sdk.preview("workflow.run", {"mode": "active", "independentReference": True,
                    "runtimeControl": True, "pauseAtAcceptedSeq": 19})
                job_id = sdk.execute(preview, sdk.approve(preview))["jobId"]
                before = helper.paused(sdk, job_id)
                proposal = helper.proposal(sdk, job_id, before)
                sdk.runtime_execution(job_id, "resume")
                completed = sdk.wait(job_id, timeout=60)
                self.assertEqual(completed["state"], "succeeded", completed.get("error"))
                receipt = sdk.runtime_parameters(job_id, proposal["proposalId"])
                self.assertEqual(receipt["status"], "accepted")
                self.assertEqual(receipt["receipt"]["appliedSeq"], "20")
                self.assertAlmostEqual(receipt["receipt"]["acceptedTimeS"], .1)
                self.assertEqual(completed["result"]["runtimeParameters"]["parameters"]["parameterSha256"],
                                 proposal["parameters"]["parameterSha256"])
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as reopened:
                receipt = reopened.runtime_parameters(job_id, proposal["proposalId"])
                self.assertEqual(receipt["receipt"]["appliedSeq"], "20")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    for name in ("solver", "native", "examples", "python"):
        parser.add_argument("--" + name, type=Path, default=Path(sys.executable) if name == "python" else None)
    ARGS, remaining = parser.parse_known_args()
    interrupt_tests.ARGS = ARGS
    unittest.main(argv=[sys.argv[0], *remaining])
