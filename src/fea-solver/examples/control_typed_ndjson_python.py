"""Pure typed-control NDJSON v2 child process used by the transport tests.

The process reads accepted opaque controller state on every request and does
not retain controller state between requests. Run with a Python executable,
the requested fault mode, and a JSON object mapping channel IDs to constants.
"""

import argparse
import base64
import json
import math
import sys
import time


PROTOCOL_VERSION = 2
CHANNEL_CATALOG = []


def quantity_unit(quantity):
    units = {
        "force": "N",
        "cable_reference_length": "m",
        "cable_initial_tension": "N",
        "cable_anchor_displacement": "m",
        "voltage": "V",
        "charge": "C",
    }
    return units[quantity]


def base_reply(request):
    return {
        "protocolVersion": PROTOCOL_VERSION,
        "requestId": request["requestId"],
        "op": request["op"],
    }


def make_reply(request, mode, constants):
    global CHANNEL_CATALOG
    if request.get("protocolVersion") != PROTOCOL_VERSION:
        raise ValueError("unsupported typed control protocol")
    operation = request.get("op")
    reply = base_reply(request)

    if operation == "handshake":
        offered = request["capabilities"]
        if offered.get("commandSchema") != "typed-values" or offered.get("pureStep") is not True:
            raise ValueError("unsupported typed control schema")
        channels = offered["channels"]
        sensors = offered["sensors"]
        CHANNEL_CATALOG = [dict(item) for item in channels]
        if mode == "bad-catalog":
            channels = [dict(item) for item in channels]
            channels[0]["unit"] = "m"
        capabilities = {
            "pureStep": mode != "bad-schema",
            "commandSchema": "other-schema" if mode == "bad-schema" else "typed-values",
            "channels": channels,
            "sensors": sensors,
        }
        reply.update({
            "ok": True,
            "controllerIdentity": "wrong-identity" if mode == "bad-identity"
                else request["controllerIdentity"],
            "configurationSha256": request["configurationSha256"],
            "deterministicSeed": request["deterministicSeed"],
            "fixedDt": request["fixedDt"],
            "commandSchema": "typed-values",
            "capabilities": capabilities,
        })
        return reply

    if operation != "compute":
        raise ValueError("unknown operation")

    if mode == "timeout":
        time.sleep(2.0)
    if mode == "exit":
        raise SystemExit(3)
    if mode == "oversized-line":
        sys.stdout.write("x" * 4096 + "\n")
        sys.stdout.flush()
        return None

    accepted_bytes = base64.b64decode(request["acceptedControllerState"], validate=True)
    accepted_count = int(accepted_bytes.decode("ascii") or "0")
    next_state = str(accepted_count + 1).encode("ascii")
    commands = []
    for channel in CHANNEL_CATALOG:
        channel_id = channel["id"]
        value = constants[channel_id]
        quantity = channel["quantity"]
        unit = channel["unit"]
        commands.append({
            "channelId": channel_id,
            "quantity": quantity,
            "unit": unit,
            "value": value,
        })

    if mode == "bad-id":
        commands[0]["channelId"] = "unexpected-channel"
    elif mode == "duplicate":
        commands[1] = dict(commands[0])
    elif mode == "missing":
        commands.pop()
    elif mode == "bad-quantity":
        commands[0]["quantity"] = "charge"
    elif mode == "invalid-unit":
        commands[0]["unit"] = "V" if commands[0]["unit"] != "V" else "m"
    elif mode == "nonfinite":
        commands[0]["value"] = math.nan
    elif mode == "wrong-step":
        reply["stepId"] = str(int(request["stepId"]) + 1)
    elif mode == "wrong-identity":
        reply["controllerIdentity"] = "other-controller/revision"
    elif mode == "bad-state":
        next_state = b"not base64"
    elif mode == "oversized-state":
        next_state = b"x" * (1024 * 1024 + 1)
    elif mode not in ("normal", "stderr", "bad-id", "duplicate", "missing",
                      "bad-quantity", "invalid-unit", "nonfinite", "wrong-step",
                      "wrong-identity", "bad-state", "oversized-state"):
        raise ValueError("unsupported fault mode")

    if mode not in ("wrong-step", "wrong-identity"):
        reply["stepId"] = request["stepId"]
    if mode != "wrong-identity":
        reply["controllerIdentity"] = request["controllerIdentity"]
    reply.update({
        "ok": True,
        "configurationSha256": request["configurationSha256"],
        "commandSchema": "typed-values",
        "commands": commands,
        "nextControllerState": "not base64!" if mode == "bad-state"
            else base64.b64encode(next_state).decode("ascii"),
    })
    if mode == "stderr":
        print("typed controller diagnostic", file=sys.stderr, flush=True)
    return reply


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode")
    parser.add_argument("commands_json")
    options = parser.parse_args()
    constants = json.loads(options.commands_json)
    if not isinstance(constants, dict) or not all(
        isinstance(key, str) and isinstance(value, (int, float)) and math.isfinite(value)
        for key, value in constants.items()
    ):
        raise ValueError("commands_json must map channel IDs to finite numbers")

    for line in sys.stdin:
        try:
            request = json.loads(line)
            response = make_reply(request, options.mode, constants)
            if response is None:
                continue
            allow_nan = options.mode == "nonfinite"
            sys.stdout.write(json.dumps(response, allow_nan=allow_nan,
                                        separators=(",", ":")) + "\n")
            sys.stdout.flush()
        except SystemExit:
            raise
        except (KeyError, TypeError, ValueError, IndexError) as error:
            print(f"typed controller rejected request: {error}", file=sys.stderr, flush=True)
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
