"""Small external SDK client. --run explicitly authorizes its fixed reference experiment."""
import argparse
import json
from pathlib import Path
import sys

from range_workflow import WorkflowClient, WorkflowError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installation", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--model", choices=("four-rod", "cantilever-beam"), default="cantilever-beam")
    parser.add_argument("--run", action="store_true", help="authorize configuration and 20-step active/passive calculation")
    args = parser.parse_args()
    root = args.installation.resolve()
    suffix = ".exe" if sys.platform == "win32" else ""
    with WorkflowClient(args.workspace.resolve(), root / "bin" / ("fea-solver" + suffix),
                        root / "bin" / ("range-workflow-native" + suffix),
                        root / "examples/control/stage9") as client:
        if not args.run:
            print(json.dumps(client.request("capabilities.query"), ensure_ascii=False))
            return 0
        for command, parameters in (
            ("workflow.configure", {"model": args.model, "dt": .005, "steps": 20, "targetDamping": .1}),
            ("workflow.run", {"mode": "both", "independentReference": True}),
        ):
            preview = client.preview(command, parameters)
            approval = client.approve(preview)  # The fixed operation was authorized by --run.
            submitted = client.execute(preview, approval)
            job = client.wait(submitted["jobId"], timeout=120)
            if job["state"] != "succeeded":
                print(json.dumps({"status": job["state"], "error": job.get("error")}))
                return 1
        print(json.dumps(client.request("results.query", {"jobId": job["jobId"]})["result"], ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except WorkflowError as error:
        print(json.dumps({"status": "error", "code": error.code, "message": str(error)}))
        raise SystemExit(1)
