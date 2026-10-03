"""Host binding for a fixed Stage 9 law across accepted native checkpoints.

The immutable gain binds its design model. Each new solver invocation has a
different native input byte hash, supplied by the workflow host through the
process environment and checked against the transport handshake. Neither the
gain nor any controller argv/script bytes change on restart.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--implementation", required=True)
    parser.add_argument("--gains", required=True)
    args = parser.parse_args()
    expected = os.environ.get("RANGE_WORKFLOW_INPUT_SHA256", "")
    if not re.fullmatch("[a-f0-9]{64}", expected):
        parser.error("Workflow host must bind the current native input digest")
    specification = importlib.util.spec_from_file_location("range_stage9_controller", args.implementation)
    implementation = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(implementation)
    try:
        gains, _ = implementation._load_json_bytes(args.gains, implementation.MAX_GAIN_BYTES)
        controller = implementation.VelocityControllerRuntime(gains)
        # Gain metadata retains its design source provenance; handshake identity
        # is the exact current input accepted by the host for this invocation.
        controller.source_sha256 = expected
        while True:
            line = sys.stdin.buffer.readline(implementation.MAX_PROTOCOL_LINE_BYTES + 1)
            if not line:
                return 0
            if len(line) > implementation.MAX_PROTOCOL_LINE_BYTES:
                raise ValueError("Controller request exceeds its byte limit")
            request = json.loads(line, object_pairs_hook=implementation._unique_object,
                                 parse_constant=implementation._reject_constant)
            response = controller.handle(request)
            sys.stdout.buffer.write(implementation._json_bytes(response) + b"\n")
            sys.stdout.buffer.flush()
    except (ValueError, OSError) as error:
        print(f"Workflow controller: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
