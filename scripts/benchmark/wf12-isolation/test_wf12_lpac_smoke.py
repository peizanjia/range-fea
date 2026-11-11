"""Actual LPAC scientific startup characterization; no ordinary-mode enablement."""
import argparse
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser()
    for name in ("workspace", "python", "launcher", "package"):
        parser.add_argument("--"+name, required=True, type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.package))
    from range_workflow import isolation
    from range_workflow.scientific_extensions import golden_cases
    from range_workflow.scientific_contract import strict_json
    from range_workflow.scientific_reference import verify_result
    from range_workflow.service import WorkflowError, file_hash
    args.workspace.mkdir(parents=True, exist_ok=True)
    data = golden_cases()[0]["input"]
    request_path, request = isolation.stage(args.workspace, args.python,
        args.package/"range_workflow/scientific_example.py", data)
    summary = {"schema": "range.wf12-smoke/v1", "requestPath": str(request_path),
               "ordinaryModeEnabled": False, "passed": False}
    try:
        receipt = isolation.launch(request_path, args.launcher)
        output = Path(receipt["output"]["path"])
        if file_hash(output) != receipt["output"]["sha256"]:
            raise WorkflowError("OUTPUT_CHANGED", "Isolated output changed")
        profile = verify_result(strict_json(output.read_bytes()), data)
        summary.update(passed=True, receipt=receipt, verificationProfile=profile)
    except Exception as error:
        summary.update(errorType=type(error).__name__, error=str(error))
    (args.workspace/"smoke-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
