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

def runtime_controller_type(implementation, runtime_parameters=None):
    class RuntimeVelocityController(implementation.VelocityControllerRuntime):
        def __init__(self, gains):
            super().__init__(gains)
            self.runtime_identity = None

        def _handshake(self, request, base):
            response = super()._handshake(request, base)
            mode = request.get("capabilities", {}).get("runtimeParameters")
            if mode is None:
                if "runtimeIdentity" in request:
                    raise ValueError("Runtime identity requires negotiated runtime parameters")
                return response
            if runtime_parameters is None:
                raise ValueError("Runtime parameter codec was not explicitly pinned by the host")
            identity = request.get("runtimeIdentity")
            if (mode != "range.runtime-parameters/v1" or type(identity) is not dict
                    or set(identity) != {"jobId", "runId", "taskId", "modelSha256", "baseControllerRevision"}
                    or identity["modelSha256"] != request["modelSnapshotSha256"]
                    or identity["baseControllerRevision"] != request["controllerRevision"]):
                raise ValueError("Invalid negotiated runtime identity")
            import uuid
            for key in ("jobId", "runId", "taskId"):
                if str(uuid.UUID(identity[key])) != identity[key] or uuid.UUID(identity[key]).int == 0:
                    raise ValueError("Runtime identity must use canonical nonnull UUIDs")
            self.runtime_identity = dict(identity)
            response["runtimeIdentity"] = dict(identity)
            response["capabilities"]["runtimeParameters"] = mode
            return response

        def _compute(self, request, base):
            extension = request.get("runtimeParameters")
            if self.runtime_identity is None:
                if extension is not None:
                    raise ValueError("Runtime parameters were not negotiated")
                return super()._compute(request, base)
            if (type(extension) is not dict or set(extension) != {"schema", "identity", "parameters"}
                    or extension["schema"] != "range.runtime-parameters/v1"
                    or extension["identity"] != self.runtime_identity):
                raise ValueError("Compute runtime identity mismatch")
            parameters = runtime_parameters.validate_parameters(extension["parameters"])
            if (parameters["fixedBaseControllerRevision"] != self.runtime_identity["baseControllerRevision"]
                    or len(parameters["actuatorLimits"]) != 1
                    or parameters["actuatorLimits"][0]["actuatorId"] != self.actuator_id):
                raise ValueError("Compute runtime parameters differ from negotiated source")
            response = super()._compute(request, base, effective_gain=parameters["velocityGainNsm"])
            response["runtimeParameters"] = extension
            return response

    return RuntimeVelocityController


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--implementation", required=True)
    parser.add_argument("--gains", required=True)
    parser.add_argument("--runtime-parameters-codec")
    args = parser.parse_args()
    expected = os.environ.get("RANGE_WORKFLOW_INPUT_SHA256", "")
    if not re.fullmatch("[a-f0-9]{64}", expected):
        parser.error("Workflow host must bind the current native input digest")
    specification = importlib.util.spec_from_file_location("range_stage9_controller", args.implementation)
    implementation = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(implementation)
    codec = None
    if args.runtime_parameters_codec:
        codec_spec = importlib.util.spec_from_file_location("range_runtime_parameters", args.runtime_parameters_codec)
        codec = importlib.util.module_from_spec(codec_spec)
        codec_spec.loader.exec_module(codec)
    try:
        gains, _ = implementation._load_json_bytes(args.gains, implementation.MAX_GAIN_BYTES)
        controller = runtime_controller_type(implementation, codec)(gains)
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
