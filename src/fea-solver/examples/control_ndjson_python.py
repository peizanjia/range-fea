"""Minimal pure, same-machine control child for ControlNdjsonTransport v1.

Launch with a Python executable plus this script as separate process arguments.
This is a protocol example, not an integrated solver controller.
"""

import json
import math
import sys


def reply(request):
    if request.get("protocolVersion") != 1 or not isinstance(request.get("requestId"), str):
        raise ValueError("unsupported control protocol")
    op = request.get("op")
    base = {"protocolVersion": 1, "requestId": request["requestId"], "op": op}
    if op == "handshake":
        offered = request["capabilities"]
        if offered.get("controlMode") != "scalar-nodal-force" or offered.get("pureStep") is not True:
            raise ValueError("unsupported control mode")
        return {**base, "ok": True, "capabilities": {
            "pureStep": True,
            "controlMode": "scalar-nodal-force",
            "actuatorIds": offered["actuatorIds"],
        }}
    if op == "compute":
        accepted = request["acceptedControllerState"]
        gain = float(accepted.get("gain", 0.0))
        if not math.isfinite(gain):
            raise ValueError("non-finite gain")
        observations = request["observations"]
        displacement = float(observations[0]["value"])
        # Pure request: only the accepted state and this observation determine
        # the command. The parent commits nextControllerState after accepting.
        return {**base, "stepId": request["stepId"],
                "commands": [{"actuatorId": "tip-force", "force": -gain * displacement,
                              "unit": "N"}],
                "nextControllerState": accepted}
    raise ValueError("unknown operation")


for line in sys.stdin:
    try:
        request = json.loads(line)
        response = reply(request)
        sys.stdout.write(json.dumps(response, allow_nan=False, separators=(",", ":")) + "\n")
        sys.stdout.flush()
    except (KeyError, TypeError, ValueError, IndexError) as error:
        print(f"control child rejected request: {error}", file=sys.stderr, flush=True)
        sys.exit(2)
