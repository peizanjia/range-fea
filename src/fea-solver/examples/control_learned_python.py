#!/usr/bin/env python3
"""Dependency-free v1 runtime for an offline learned velocity policy.

The companion trainer distills the collocated law ``force = -gain * velocity``
into a small MLP. This process only evaluates the exported JSON weights. Its
compute operation is pure: accepted controller state is returned for the
solver to commit, and this process never advances a hidden sample counter.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from typing import Any, TextIO


PROTOCOL_VERSION = 1
POLICY_FORMAT = "range-fea-learned-velocity-policy-v1"
MAX_POLICY_BYTES = 1024 * 1024
MAX_PROTOCOL_LINE_BYTES = 1024 * 1024
MAX_ID_LENGTH = 256
MAX_WEIGHT_MAGNITUDE = 1.0e6
MAX_PHYSICAL_SCALE = 1.0e12
MAX_OUTPUT_SCALE = MAX_PHYSICAL_SCALE * MAX_PHYSICAL_SCALE
# QJsonValue stores numbers as IEEE-754 doubles, so state counts must remain
# exactly representable when serialized by the native v1 transport.
MAX_STATE_COUNT = 0x1FFFFFFFFFFFFF
MAX_STEP = 0xFFFFFFFFFFFFFFFF
HIDDEN_WIDTH = 8


class ControllerError(ValueError):
    """Invalid learned policy or protocol request."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ControllerError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ControllerError(f"non-finite JSON number: {value}")


def _finite(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ControllerError(f"{field} must be a finite number")
    try:
        result = float(value)
    except (OverflowError, ValueError) as error:
        raise ControllerError(f"{field} must be a finite number") from error
    if not math.isfinite(result):
        raise ControllerError(f"{field} must be a finite number")
    return result


def _valid_id(value: Any, field: str) -> str:
    if (not isinstance(value, str) or not value or len(value) > MAX_ID_LENGTH or
            "\n" in value or "\r" in value):
        raise ControllerError(f"{field} must be a non-empty ID of at most {MAX_ID_LENGTH} characters")
    return value


def _valid_sha256(value: Any, field: str) -> str:
    if (not isinstance(value, str) or len(value) != 64 or
            any(char not in "0123456789abcdef" for char in value)):
        raise ControllerError(f"{field} must be a lowercase SHA-256 digest")
    return value


def _step_number(value: Any) -> int:
    if (not isinstance(value, str) or not value or len(value) > 20 or
            not value.isascii() or not value.isdecimal()):
        raise ControllerError("stepId must be an unsigned decimal string")
    result = int(value)
    if result > MAX_STEP:
        raise ControllerError("stepId exceeds the supported unsigned range")
    return result


def _vector(value: Any, length: int, field: str) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != length:
        raise ControllerError(f"{field} must have length {length}")
    result = tuple(_finite(item, field) for item in value)
    if any(abs(item) > MAX_WEIGHT_MAGNITUDE for item in result):
        raise ControllerError(f"{field} entries exceed the supported magnitude")
    return result


def _matrix(value: Any, rows: int, columns: int, field: str) -> tuple[tuple[float, ...], ...]:
    if not isinstance(value, list) or len(value) != rows:
        raise ControllerError(f"{field} must have shape {rows}x{columns}")
    return tuple(_vector(row, columns, field) for row in value)


@dataclass(frozen=True)
class LearnedVelocityPolicy:
    """Validated policy with immutable, bounded dense-layer parameters."""

    sensor_id: str
    actuator_id: str
    velocity_scale: float
    output_scale: float
    gain: float
    hidden_weights: tuple[tuple[float, ...], ...]
    hidden_bias: tuple[float, ...]
    output_weights: tuple[float, ...]
    output_bias: float

    @classmethod
    def from_json(cls, source: Any) -> "LearnedVelocityPolicy":
        if not isinstance(source, dict):
            raise ControllerError("policy root must be an object")
        expected = {"format", "version", "kind", "controlMode", "sensorCatalog",
                    "actuatorCatalog", "normalization", "network", "distillation"}
        if set(source) != expected:
            raise ControllerError("policy has missing or unsupported root fields")
        if (source["format"] != POLICY_FORMAT or source["version"] != 1 or
                isinstance(source["version"], bool) or
                source["kind"] != "supervised-collocated-velocity-gain-distillation" or
                source["controlMode"] != "scalar-nodal-force"):
            raise ControllerError("unsupported learned policy format")

        sensors = source["sensorCatalog"]
        actuators = source["actuatorCatalog"]
        if (not isinstance(sensors, list) or len(sensors) != 1 or
                not isinstance(sensors[0], dict) or set(sensors[0]) != {"sensorId", "unit"}):
            raise ControllerError("policy must define one ordered velocity sensor")
        if (not isinstance(actuators, list) or len(actuators) != 1 or
                not isinstance(actuators[0], dict) or
                set(actuators[0]) != {"actuatorId", "unit"}):
            raise ControllerError("policy must define one ordered force actuator")
        sensor_id = _valid_id(sensors[0]["sensorId"], "sensorId")
        actuator_id = _valid_id(actuators[0]["actuatorId"], "actuatorId")
        if sensors[0]["unit"] != "m/s" or actuators[0]["unit"] != "N":
            raise ControllerError("policy sensor and actuator units must be m/s and N")

        normalization = source["normalization"]
        norm_keys = {"inputOffset", "inputScale", "inputDomain", "outputOffset", "outputScale"}
        if not isinstance(normalization, dict) or set(normalization) != norm_keys:
            raise ControllerError("policy normalization has missing or unsupported fields")
        input_offset = _finite(normalization["inputOffset"], "normalization.inputOffset")
        velocity_scale = _finite(normalization["inputScale"], "normalization.inputScale")
        output_offset = _finite(normalization["outputOffset"], "normalization.outputOffset")
        output_scale = _finite(normalization["outputScale"], "normalization.outputScale")
        domain = normalization["inputDomain"]
        if (input_offset != 0.0 or output_offset != 0.0 or velocity_scale <= 0.0 or
                velocity_scale > MAX_PHYSICAL_SCALE or output_scale <= 0.0 or
                output_scale > MAX_OUTPUT_SCALE or not isinstance(domain, list) or
                len(domain) != 2 or _finite(domain[0], "normalization.inputDomain[0]") != -velocity_scale or
                _finite(domain[1], "normalization.inputDomain[1]") != velocity_scale):
            raise ControllerError("policy normalization scales or input domain are invalid")

        distillation = source["distillation"]
        distill_keys = {"law", "gainNSecondsPerMeter", "trainingSamples", "holdoutSamples",
                        "epochs", "seed"}
        if not isinstance(distillation, dict) or set(distillation) != distill_keys:
            raise ControllerError("policy distillation metadata has missing or unsupported fields")
        gain = _finite(distillation["gainNSecondsPerMeter"], "distillation gain")
        if (distillation["law"] != "force=-gain*velocity" or gain <= 0.0 or
                gain > MAX_PHYSICAL_SCALE or not math.isclose(output_scale, gain * velocity_scale,
                                                              rel_tol=1.0e-12, abs_tol=0.0)):
            raise ControllerError("policy distillation gain and normalization do not agree")
        for key, minimum, maximum in (("trainingSamples", 2, 1_000_000),
                                      ("holdoutSamples", 2, 1_000_000),
                                      ("epochs", 1, 1_000_000),
                                      ("seed", 0, 0x7FFFFFFFFFFFFFFF)):
            value = distillation[key]
            if (isinstance(value, bool) or not isinstance(value, int) or
                    value < minimum or value > maximum):
                raise ControllerError(f"distillation.{key} is outside the supported range")

        network = source["network"]
        if not isinstance(network, dict) or set(network) != {"activation", "layerSizes", "layers"}:
            raise ControllerError("policy network has missing or unsupported fields")
        layer_sizes = network["layerSizes"]
        if (network["activation"] != "tanh" or not isinstance(layer_sizes, list) or
                len(layer_sizes) != 3 or any(isinstance(size, bool) or not isinstance(size, int)
                                             for size in layer_sizes) or
                layer_sizes != [1, HIDDEN_WIDTH, 1]):
            raise ControllerError("policy network must use the supported 1-8-1 tanh architecture")
        layers = network["layers"]
        if not isinstance(layers, list) or len(layers) != 2:
            raise ControllerError("policy network must contain exactly two dense layers")
        for layer in layers:
            if not isinstance(layer, dict) or set(layer) != {"weights", "bias"}:
                raise ControllerError("dense layers must contain weights and bias only")
        hidden_weights = _matrix(layers[0]["weights"], HIDDEN_WIDTH, 1, "hidden weights")
        hidden_bias = _vector(layers[0]["bias"], HIDDEN_WIDTH, "hidden bias")
        output_weights_matrix = _matrix(layers[1]["weights"], 1, HIDDEN_WIDTH, "output weights")
        output_bias_vector = _vector(layers[1]["bias"], 1, "output bias")
        if (any(row[0] <= 0.0 for row in hidden_weights) or
                any(value != 0.0 for value in hidden_bias) or
                any(value >= 0.0 for value in output_weights_matrix[0]) or
                output_bias_vector[0] != 0.0):
            raise ControllerError("tanh policy weights must preserve odd, opposing-sign feedback")
        return cls(sensor_id, actuator_id, velocity_scale, output_scale, gain,
                   hidden_weights, hidden_bias, output_weights_matrix[0], output_bias_vector[0])

    def force_for_velocity(self, velocity: float) -> float:
        value = _finite(velocity, "velocity observation")
        if value < -self.velocity_scale or value > self.velocity_scale:
            raise ControllerError("velocity observation is outside the trained input domain")
        normalized = value / self.velocity_scale
        hidden = tuple(math.tanh(row[0] * normalized + bias)
                       for row, bias in zip(self.hidden_weights, self.hidden_bias))
        output = math.fsum(weight * activation
                           for weight, activation in zip(self.output_weights, hidden)) + self.output_bias
        force = self.output_scale * output
        if not math.isfinite(force):
            raise ControllerError("learned policy produced a non-finite force")
        return force


def load_policy(path: str) -> LearnedVelocityPolicy:
    try:
        with open(path, "rb") as stream:
            raw = stream.read(MAX_POLICY_BYTES + 1)
    except OSError as error:
        raise ControllerError(f"cannot read policy file: {error}") from error
    if not raw or len(raw) > MAX_POLICY_BYTES:
        raise ControllerError(f"policy file must contain 1..{MAX_POLICY_BYTES} bytes")
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object,
                           parse_constant=_reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise ControllerError(f"invalid policy JSON: {error}") from error
    return LearnedVelocityPolicy.from_json(value)


class LearnedVelocityRuntime:
    """Pure v1 request handler for a single learned velocity-to-force policy."""

    def __init__(self, policy: LearnedVelocityPolicy | dict[str, Any]):
        self.policy = (policy if isinstance(policy, LearnedVelocityPolicy)
                       else LearnedVelocityPolicy.from_json(policy))
        self._identity: dict[str, str] | None = None
        self._sensor_units: dict[str, str] = {}

    def handle(self, request: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(request, dict):
            raise ControllerError("request must be a JSON object")
        version = request.get("protocolVersion")
        request_id = request.get("requestId")
        op = request.get("op")
        if (isinstance(version, bool) or not isinstance(version, int) or version != PROTOCOL_VERSION or
                not isinstance(request_id, str) or not request_id or len(request_id) > MAX_ID_LENGTH or
                "\n" in request_id or "\r" in request_id):
            raise ControllerError("unsupported protocol version or invalid requestId")
        base = {"protocolVersion": PROTOCOL_VERSION, "requestId": request_id, "op": op}
        if op == "handshake":
            return self._handshake(request, base)
        if op == "compute":
            return self._compute(request, base)
        raise ControllerError("unknown operation")

    def _handshake(self, request: dict[str, Any], base: dict[str, Any]) -> dict[str, Any]:
        offered = request.get("capabilities")
        if (not isinstance(offered, dict) or offered.get("pureStep") is not True or
                offered.get("controlMode") != "scalar-nodal-force"):
            raise ControllerError("unsupported v1 control mode")
        sensors = offered.get("sensors")
        actuators = offered.get("actuatorIds")
        if not isinstance(sensors, list) or not 1 <= len(sensors) <= 64:
            raise ControllerError("handshake sensor catalog must contain 1..64 entries")
        sensor_units: dict[str, str] = {}
        for item in sensors:
            if not isinstance(item, dict) or set(item) != {"sensorId", "unit"}:
                raise ControllerError("handshake sensor entries must contain sensorId and unit")
            sensor_id = _valid_id(item["sensorId"], "sensorId")
            unit = item["unit"]
            if (not isinstance(unit, str) or unit not in {"m", "m/s", "m/s^2"} or
                    sensor_id in sensor_units):
                raise ControllerError("handshake has a duplicate sensor ID or unsupported sensor unit")
            sensor_units[sensor_id] = unit
        if sensor_units.get(self.policy.sensor_id) != "m/s":
            raise ControllerError("handshake must offer the configured velocity sensor in m/s")
        if not isinstance(actuators, list) or actuators != [self.policy.actuator_id]:
            raise ControllerError("handshake actuator catalog does not match the ordered policy catalog")
        controller_id = _valid_id(request.get("controllerId"), "controllerId")
        revision = _valid_sha256(request.get("controllerRevision"), "controllerRevision")
        model_hash = _valid_sha256(request.get("modelSnapshotSha256"), "modelSnapshotSha256")
        seed = request.get("deterministicSeed")
        if (not isinstance(seed, str) or len(seed) > 20 or not seed.isascii() or
                not seed.isdecimal() or int(seed) > MAX_STEP):
            raise ControllerError("deterministicSeed must be an unsigned decimal string")
        identity = {"controllerId": controller_id, "controllerRevision": revision,
                    "modelSnapshotSha256": model_hash, "deterministicSeed": seed}
        if self._identity is not None and (
                identity != self._identity or
                list(sensor_units.items()) != list(self._sensor_units.items())):
            raise ControllerError("handshake identity or sensor catalog cannot change during a runtime session")
        self._identity = identity
        self._sensor_units = sensor_units
        return {**base, "ok": True,
                "capabilities": {"pureStep": True, "controlMode": "scalar-nodal-force",
                                 "actuatorIds": [self.policy.actuator_id]},
                **identity}

    def _compute(self, request: dict[str, Any], base: dict[str, Any]) -> dict[str, Any]:
        if self._identity is None:
            raise ControllerError("handshake is required before compute")
        for key, value in self._identity.items():
            if request.get(key) != value:
                raise ControllerError(f"compute identity mismatch: {key}")
        step_text = request.get("stepId")
        step = _step_number(step_text)
        time = _finite(request.get("time"), "time")
        dt = _finite(request.get("dt"), "dt")
        if time < 0.0 or dt <= 0.0:
            raise ControllerError("time must be nonnegative and dt positive")
        observations = request.get("observations")
        if (not isinstance(observations, list) or
                len(observations) != len(self._sensor_units)):
            raise ControllerError("compute observation catalog is incomplete or oversized")
        observed: dict[str, float] = {}
        for observation in observations:
            if not isinstance(observation, dict):
                raise ControllerError("compute observations must be objects")
            sensor_id = observation.get("sensorId")
            if (not isinstance(sensor_id, str) or sensor_id not in self._sensor_units or
                    sensor_id in observed or observation.get("unit") != self._sensor_units[sensor_id]):
                raise ControllerError("observation ID/unit does not match the handshaken sensor catalog")
            observed[sensor_id] = _finite(observation.get("value"), f"observation {sensor_id}")
        if set(observed) != set(self._sensor_units):
            raise ControllerError("compute observation IDs do not match the handshaken sensor catalog")
        velocity = observed[self.policy.sensor_id]
        state = request.get("acceptedControllerState")
        if not isinstance(state, dict) or set(state) - {"sampleCount", "lastStepId"}:
            raise ControllerError("acceptedControllerState has an unsupported shape")
        count = state.get("sampleCount", 0)
        if (isinstance(count, bool) or not isinstance(count, int) or
                count < 0 or count >= MAX_STATE_COUNT):
            raise ControllerError("acceptedControllerState.sampleCount must be a nonnegative bounded integer")
        previous_step = state.get("lastStepId")
        if count == 0:
            if previous_step is not None:
                raise ControllerError("fresh controller state cannot contain lastStepId")
        elif step <= _step_number(previous_step):
            raise ControllerError("stepId must advance beyond the last accepted controller step")
        force = self.policy.force_for_velocity(velocity)
        next_state = {"sampleCount": count + 1, "lastStepId": step_text}
        return {**base, "stepId": step_text, **self._identity,
                "commands": [{"actuatorId": self.policy.actuator_id, "force": force, "unit": "N"}],
                "nextControllerState": next_state}


def serve(policy_path: str, stdin: TextIO = sys.stdin, stdout: TextIO = sys.stdout,
          stderr: TextIO = sys.stderr) -> int:
    try:
        runtime = LearnedVelocityRuntime(load_policy(policy_path))
    except ControllerError as error:
        print(f"Learned velocity controller: {error}", file=stderr, flush=True)
        return 2
    while True:
        line = stdin.readline(MAX_PROTOCOL_LINE_BYTES + 1)
        if line == "":
            break
        if len(line.encode("utf-8")) > MAX_PROTOCOL_LINE_BYTES:
            print("Learned velocity controller: request exceeds the line limit", file=stderr, flush=True)
            return 2
        try:
            request = json.loads(line, object_pairs_hook=_unique_object,
                                 parse_constant=_reject_constant)
            response = runtime.handle(request)
            encoded = json.dumps(response, allow_nan=False, separators=(",", ":"))
            if len(encoded.encode("utf-8")) > MAX_PROTOCOL_LINE_BYTES:
                raise ControllerError("response exceeds the line limit")
            stdout.write(encoded + "\n")
            stdout.flush()
        except (ControllerError, TypeError, ValueError, RecursionError) as error:
            print(f"Learned velocity controller: {error}", file=stderr, flush=True)
            return 2
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", "--weights", dest="policy", required=True,
                        help="exported learned-policy JSON file")
    args = parser.parse_args(argv)
    return serve(args.policy)


if __name__ == "__main__":
    raise SystemExit(main())
