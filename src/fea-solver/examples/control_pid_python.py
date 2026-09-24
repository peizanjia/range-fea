"""Pure displacement PID example for the Stage-6 scalar-nodal-force protocol.

Usage: python control_pid_python.py KP KI KD
Model catalog: one displacement sensor and one nodal-force actuator. Gains have
units N/m, N/(m*s), and N*s/m respectively. The FE solver applies actuator
limits after this controller; this example does not implement anti-windup.
"""

import json
import math
import sys


if len(sys.argv) != 4:
    raise SystemExit("usage: control_pid_python.py KP KI KD")
kp, ki, kd = (float(value) for value in sys.argv[1:])
if not all(math.isfinite(value) and value >= 0.0 for value in (kp, ki, kd)):
    raise SystemExit("PID gains must be finite and nonnegative")


def reply(request):
    if request.get("protocolVersion") != 1 or not isinstance(request.get("requestId"), str):
        raise ValueError("unsupported protocol")
    op = request.get("op")
    base = {"protocolVersion": 1, "requestId": request["requestId"], "op": op}
    if op == "handshake":
        offered = request["capabilities"]
        sensors = offered["sensors"]
        actuators = offered["actuatorIds"]
        if offered.get("pureStep") is not True or offered.get("controlMode") != "scalar-nodal-force":
            raise ValueError("unsupported control mode")
        if len(sensors) != 1 or sensors[0]["unit"] != "m" or len(actuators) != 1:
            raise ValueError("PID example requires one displacement sensor and one force actuator")
        return {**base, "ok": True, "capabilities": {
            "pureStep": True, "controlMode": "scalar-nodal-force", "actuatorIds": actuators,
        }, "controllerId": request["controllerId"],
            "controllerRevision": request["controllerRevision"],
            "modelSnapshotSha256": request["modelSnapshotSha256"],
            "deterministicSeed": request["deterministicSeed"]}
    if op == "compute":
        state = request["acceptedControllerState"]
        dt = float(request["dt"])
        if not math.isfinite(dt) or dt <= 0.0:
            raise ValueError("invalid time step")
        observations = request["observations"]
        if len(observations) != 1 or observations[0]["unit"] != "m":
            raise ValueError("invalid displacement observation")
        displacement = float(observations[0]["value"])
        previous_error = float(state.get("previousError", 0.0))
        integral = float(state.get("integral", 0.0))
        count = int(state.get("sampleCount", 0))
        if not all(math.isfinite(value) for value in (displacement, previous_error, integral)):
            raise ValueError("non-finite PID state")
        error = -displacement
        next_integral = integral + error * dt
        derivative = 0.0 if count == 0 else (error - previous_error) / dt
        force = kp * error + ki * next_integral + kd * derivative
        if not math.isfinite(force):
            raise ValueError("non-finite PID command")
        if actuator_id is None:
            raise ValueError("handshake required")
        return {**base, "stepId": request["stepId"],
                "commands": [{"actuatorId": actuator_id,
                              "force": force, "unit": "N"}],
                "nextControllerState": {"integral": next_integral,
                                        "previousError": error, "sampleCount": count + 1}}
    raise ValueError("unknown operation")


actuator_id = None
for line in sys.stdin:
    try:
        request = json.loads(line)
        if request.get("op") == "handshake":
            actuator_id = request["capabilities"]["actuatorIds"][0]
        response = reply(request)
        sys.stdout.write(json.dumps(response, allow_nan=False, separators=(",", ":")) + "\n")
        sys.stdout.flush()
    except (KeyError, TypeError, ValueError, IndexError) as error:
        print(f"PID controller rejected request: {error}", file=sys.stderr, flush=True)
        sys.exit(2)
