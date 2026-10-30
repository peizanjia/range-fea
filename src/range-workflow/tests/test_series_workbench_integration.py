"""Real two-job FE overlay/reference/error and complete export verification."""
import argparse
import base64
import csv
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_workflow import WorkflowClient
from range_workflow.model_sources import register_snapshot_source
from range_workflow.service import file_hash
from test_imported_workflow import _make_catalog

ARGS = None


class SeriesWorkbenchIntegration(unittest.TestCase):
    def test_actual_two_job_native_overlay_reference_energy_and_raw_export(self):
        if ARGS is None or ARGS.solver is None:
            self.skipTest("Explicit native paths required")
        with tempfile.TemporaryDirectory(prefix="range-series-workbench-") as temporary:
            base = Path(temporary); snapshot = base / "source.rbm"
            prepared = subprocess.run([str(ARGS.native), "prepare", "--input", str(ARGS.examples / "stage9_truss4.ram"),
                "--output", str(snapshot), "--steps", "20", "--output-frequency", "1", "--dt", ".005"],
                capture_output=True, timeout=60)
            self.assertEqual(prepared.returncode, 0, prepared.stderr.decode(errors="replace"))
            _, ref = _make_catalog(base / "catalog", snapshot, file_hash(snapshot))
            source = register_snapshot_source(base / "workspace", base / "catalog", ref, ARGS.native)
            with WorkflowClient(base / "workspace", ARGS.solver, ARGS.native, ARGS.examples, ARGS.python) as sdk:
                jobs = []
                for damping in (.05, .1):
                    configure = sdk.preview("workflow.configure", {"dt": .005, "steps": 20,
                        "targetDamping": damping, "sourceId": source["sourceId"]})
                    accepted = sdk.execute(configure, sdk.approve(configure))
                    self.assertEqual(sdk.wait(accepted["jobId"])["state"], "succeeded")
                    preview = sdk.preview("workflow.run", {"mode": "both", "independentReference": True})
                    launched = sdk.execute(preview, sdk.approve(preview))
                    done = sdk.wait(launched["jobId"], timeout=90)
                    self.assertEqual(done["state"], "succeeded", done.get("error"))
                    jobs.append(done["jobId"])
                selected, reference = [], None
                for job_id in jobs:
                    for mode in ("active", "passive"):
                        catalog = sdk.series_sources(job_id, mode)
                        velocity = next(entry for entry in catalog["channels"]
                                        if entry["selector"]["channelId"].endswith(":velocity:X")
                                        and entry["selector"]["channelId"].split(":")[1] ==
                                        str(source["descriptor"]["ports"]["velocity"]["nodePhysicalId"]))
                        selected.append(velocity["selector"])
                        if reference is None and mode == "active":
                            reference = velocity.get("reference")
                overlay = sdk.series_plot(selected, limit=16)
                self.assertEqual(len(overlay["traces"]), 4)
                self.assertEqual(overlay["axes"]["y"]["unit"], "m/s")
                for trace in overlay["traces"]:
                    self.assertEqual(trace["rawSampleCount"], 20)
                    self.assertLessEqual(trace["displaySampleCount"], 16)
                self.assertIsNotNone(reference)
                comparison = sdk.series_plot([selected[0], reference], view="error", limit=16)
                self.assertLessEqual(max(abs(point["y"]) for point in comparison["traces"][0]["points"]), 1e-7)
                energy = sdk.series_plot([selected[0]], view="energy", limit=16)
                self.assertEqual(energy["axes"]["y"]["unit"], "J")
                self.assertEqual(len(energy["traces"]), 1, energy["unavailable"])
                self.assertTrue(energy["export"]["available"])
                work = sdk.series_plot([selected[0], selected[1]], view="work", limit=16)
                self.assertEqual(len(work["traces"]), 1, work["unavailable"])
                self.assertEqual(len(work["unavailable"]), 1)
                self.assertEqual(work["traces"][0]["unit"], "J")
                full_work = sdk.series_raw(work["export"]["sources"][0], limit=200)
                self.assertEqual(full_work["rawSampleCount"], 20)
                first_job = sdk.request("jobs.query", {"jobId": jobs[0]})["job"]
                reference_report = json.loads(Path(first_job["result"]["referencePath"]).read_bytes())
                self.assertAlmostEqual(full_work["points"][-1]["value"], reference_report["controllerWorkJ"], delta=1e-8)
                exported = sdk.series_export(overlay["export"]["sources"])
                raw = bytearray(); offset = 0
                while offset < exported["bytes"]:
                    chunk = sdk.series_export_chunk(exported["exportId"], offset)
                    block = base64.b64decode(chunk["data"], validate=True)
                    self.assertEqual(len(block), chunk["length"])
                    self.assertEqual(chunk["offset"], offset)
                    raw.extend(block); offset += len(block)
                self.assertEqual(hashlib.sha256(raw).hexdigest(), exported["sha256"])
                rows = list(csv.DictReader(io.StringIO(raw.decode("utf-8"))))
                self.assertEqual(len(rows), 80)
                self.assertEqual(exported["count"], 80)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    for name in ("solver", "native", "examples", "python"):
        parser.add_argument("--" + name, type=Path, default=Path(sys.executable) if name == "python" else None)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
