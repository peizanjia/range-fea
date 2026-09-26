#!/usr/bin/env python3
"""Train and export a small MLP distillation of a collocated velocity gain.

PyTorch is needed only for this offline training command. The exported policy
is evaluated at runtime by control_learned_python.py using the Python standard
library alone. This is supervised approximation of one scalar feedback law;
it makes no closed-loop stability claim.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import sys
from pathlib import Path
from typing import Any


POLICY_FORMAT = "range-fea-learned-velocity-policy-v1"
HIDDEN_WIDTH = 8
TRAINING_SAMPLES = 257
HOLDOUT_SAMPLES = 1025
MAX_GAIN = 1.0e12
MAX_VELOCITY_SCALE = 1.0e12
MAX_EPOCHS = 1_000_000
MAX_SEED = 0x7FFFFFFFFFFFFFFF


def _runtime_module() -> Any:
    path = Path(__file__).with_name("control_learned_python.py")
    spec = importlib.util.spec_from_file_location("range_fea_control_learned", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load dependency-free policy runtime")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _check_arguments(args: argparse.Namespace) -> None:
    if not math.isfinite(args.gain) or args.gain <= 0.0 or args.gain > MAX_GAIN:
        raise ValueError(f"--gain must be positive, finite, and at most {MAX_GAIN:g}")
    if (not math.isfinite(args.velocity_scale) or args.velocity_scale <= 0.0 or
            args.velocity_scale > MAX_VELOCITY_SCALE):
        raise ValueError(f"--velocity-scale must be positive, finite, and at most {MAX_VELOCITY_SCALE:g}")
    if isinstance(args.epochs, bool) or args.epochs < 1 or args.epochs > MAX_EPOCHS:
        raise ValueError(f"--epochs must be between 1 and {MAX_EPOCHS}")
    if isinstance(args.seed, bool) or args.seed < 0 or args.seed > MAX_SEED:
        raise ValueError(f"--seed must be between 0 and {MAX_SEED}")
    for field in ("sensor_id", "actuator_id"):
        value = getattr(args, field)
        if not value or len(value) > 256 or "\n" in value or "\r" in value:
            raise ValueError(f"--{field.replace('_', '-')} must be a non-empty ID of at most 256 characters")


def train(args: argparse.Namespace) -> dict[str, Any]:
    """Train, export, and return a concise numerical report."""
    _check_arguments(args)
    try:
        import torch
        from torch import nn
    except ImportError as error:
        raise RuntimeError("PyTorch is required only for offline policy training") from error

    torch.use_deterministic_algorithms(True)
    torch.set_num_threads(1)
    torch.manual_seed(args.seed)
    torch.set_default_dtype(torch.float64)

    class OddTanhPolicy(nn.Module):
        """Bias-free odd MLP whose positive parameters preserve force direction."""

        def __init__(self) -> None:
            super().__init__()
            hidden_seed = math.log(math.expm1(0.1))
            output_seed = math.log(math.expm1(1.25))
            self.raw_hidden = nn.Parameter(
                torch.full((HIDDEN_WIDTH,), hidden_seed, dtype=torch.float64) +
                0.01 * torch.randn(HIDDEN_WIDTH, dtype=torch.float64))
            self.raw_output = nn.Parameter(
                torch.full((HIDDEN_WIDTH,), output_seed, dtype=torch.float64) +
                0.01 * torch.randn(HIDDEN_WIDTH, dtype=torch.float64))

        def forward(self, value: Any) -> Any:
            hidden_gain = torch.nn.functional.softplus(self.raw_hidden)
            output_gain = torch.nn.functional.softplus(self.raw_output)
            hidden = torch.tanh(value * hidden_gain.reshape(1, -1))
            return hidden @ (-output_gain.reshape(-1, 1))

    model = OddTanhPolicy()
    optimizer = torch.optim.Adam(model.parameters(), lr=0.01)
    train_x = torch.linspace(-1.0, 1.0, TRAINING_SAMPLES, dtype=torch.float64).reshape(-1, 1)
    train_y = -train_x
    for _ in range(args.epochs):
        optimizer.zero_grad(set_to_none=True)
        loss = torch.mean((model(train_x) - train_y) ** 2)
        loss.backward()
        optimizer.step()

    # A shifted grid avoids reusing the exact training samples in the report.
    holdout_x = (torch.arange(HOLDOUT_SAMPLES, dtype=torch.float64) + 0.5)
    holdout_x = (2.0 * holdout_x / HOLDOUT_SAMPLES - 1.0).reshape(-1, 1)
    gain = float(args.gain)
    velocity_scale = float(args.velocity_scale)
    output_scale = gain * velocity_scale
    if not math.isfinite(output_scale) or output_scale > MAX_GAIN * MAX_VELOCITY_SCALE:
        raise ValueError("gain and velocity scale produce an unsupported output scale")

    hidden_weights = torch.nn.functional.softplus(model.raw_hidden).reshape(HIDDEN_WIDTH, 1)
    output_weights = -torch.nn.functional.softplus(model.raw_output).reshape(1, HIDDEN_WIDTH)
    artifact = {
        "format": POLICY_FORMAT,
        "version": 1,
        "kind": "supervised-collocated-velocity-gain-distillation",
        "controlMode": "scalar-nodal-force",
        "sensorCatalog": [{"sensorId": args.sensor_id, "unit": "m/s"}],
        "actuatorCatalog": [{"actuatorId": args.actuator_id, "unit": "N"}],
        "normalization": {"inputOffset": 0.0, "inputScale": velocity_scale,
                          "inputDomain": [-velocity_scale, velocity_scale],
                          "outputOffset": 0.0, "outputScale": output_scale},
        "network": {
            "activation": "tanh",
            "layerSizes": [1, HIDDEN_WIDTH, 1],
            "layers": [
                {"weights": hidden_weights.detach().cpu().tolist(),
                 "bias": [0.0 for _ in range(HIDDEN_WIDTH)]},
                {"weights": output_weights.detach().cpu().tolist(), "bias": [0.0]},
            ],
        },
        "distillation": {"law": "force=-gain*velocity",
                         "gainNSecondsPerMeter": gain,
                         "trainingSamples": TRAINING_SAMPLES,
                         "holdoutSamples": HOLDOUT_SAMPLES,
                         "epochs": args.epochs, "seed": args.seed},
    }

    runtime_module = _runtime_module()
    policy = runtime_module.LearnedVelocityPolicy.from_json(artifact)
    with torch.no_grad():
        torch_force = model(holdout_x)[:, 0] * output_scale
        target_force = -holdout_x[:, 0] * output_scale
        exported_force = torch.tensor(
            [policy.force_for_velocity(float(value) * velocity_scale) for value in holdout_x[:, 0]],
            dtype=torch.float64)
        target_error = exported_force - target_force
        parity_error = exported_force - torch_force
        max_error = float(torch.max(torch.abs(target_error)))
        rms_error = float(torch.sqrt(torch.mean(target_error ** 2)))
        parity_max = float(torch.max(torch.abs(parity_error)))
        parity_rms = float(torch.sqrt(torch.mean(parity_error ** 2)))

    output_path = Path(args.output)
    serialized = json.dumps(artifact, allow_nan=False, indent=2) + "\n"
    output_path.write_text(serialized, encoding="utf-8")
    return {
        "output": str(output_path),
        "format": POLICY_FORMAT,
        "holdout": {"samples": HOLDOUT_SAMPLES, "maxAbsErrorN": max_error,
                    "rmsErrorN": rms_error},
        "torchVsExported": {"maxAbsDifferenceN": parity_max,
                            "rmsDifferenceN": parity_rms},
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gain", required=True, type=float,
                        help="positive collocated velocity gain in N*s/m")
    parser.add_argument("--sensor-id", required=True, help="ordered velocity sensor ID")
    parser.add_argument("--actuator-id", required=True, help="ordered nodal-force actuator ID")
    parser.add_argument("--velocity-scale", required=True, type=float,
                        help="physical velocity domain is [-scale, +scale] m/s")
    parser.add_argument("--output", required=True, help="portable policy JSON output path")
    parser.add_argument("--epochs", required=True, type=int, help="Adam training epochs")
    parser.add_argument("--seed", required=True, type=int, help="deterministic CPU training seed")
    args = parser.parse_args(argv)
    try:
        report = train(args)
    except (ValueError, RuntimeError, OSError) as error:
        print(f"learned policy trainer: {error}", file=sys.stderr, flush=True)
        return 2
    print(json.dumps(report, allow_nan=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
