"""Small JSON protocol harness for trusted, versioned workflow extensions."""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path
import sys


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _read(path: Path, maximum: int):
    raw = path.read_bytes()
    if len(raw) > maximum:
        raise ValueError("protocol input exceeds its byte limit")
    return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=_unique_object,
                      parse_constant=lambda _value: (_ for _ in ()).throw(ValueError("non-finite")))


def _series(value):
    if not isinstance(value, dict) or set(value) != {"times", "values", "timeUnit", "valueUnit"}:
        raise ValueError("input must follow range.extension/v1/numeric-series")
    times, values = value["times"], value["values"]
    if (not isinstance(times, list) or not isinstance(values, list) or
            not 1 <= len(times) <= 100000 or len(values) != len(times)):
        raise ValueError("input series dimensions are invalid")
    for name, array in (("times", times), ("values", values)):
        for item in array:
            if isinstance(item, bool) or type(item) not in (int, float) or not math.isfinite(item) or abs(item) > 1e100:
                raise ValueError(f"{name} contains an invalid number")
    if any(right <= left for left, right in zip(times, times[1:])):
        raise ValueError("times must be strictly increasing")
    for unit in (value["timeUnit"], value["valueUnit"]):
        if (not isinstance(unit, str) or not unit or len(unit) > 32 or
                not unit[0].isascii() or not unit[0].isalnum() or
                any(not (x.isascii() and (x.isalnum() or x in "./*^_-")) for x in unit)):
            raise ValueError("input unit is invalid")
    return value


def _result(value, series):
    required = {"rms", "peakAbs", "peakTime", "timeUnit", "valueUnit"}
    if not isinstance(value, dict) or set(value) != required:
        raise ValueError("extension output must follow range.extension/v1/statistics")
    for name in ("rms", "peakAbs", "peakTime"):
        item = value[name]
        if isinstance(item, bool) or type(item) not in (int, float) or not math.isfinite(item) or abs(item) > 1e100:
            raise ValueError(f"output {name} is not finite")
    if value["rms"] < 0 or value["peakAbs"] < 0:
        raise ValueError("RMS and absolute peak must be nonnegative")
    if value["timeUnit"] != series["timeUnit"] or value["valueUnit"] != series["valueUnit"]:
        raise ValueError("output units differ from input units")
    return value


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--test", action="store_true")
    mode.add_argument("--run", action="store_true")
    args = parser.parse_args(argv)
    spec = importlib.util.spec_from_file_location("range_approved_extension", args.source)
    if spec is None or spec.loader is None:
        raise ValueError("extension source could not be loaded")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    function = getattr(module, "process", None)
    if not callable(function):
        raise ValueError("extension must export process(series)")
    if args.test:
        test_data = _read(Path(args.input), 96 * 1024)
        if not isinstance(test_data, dict) or set(test_data) != {"cases"}:
            raise ValueError("test input must contain cases")
        cases = test_data["cases"]
        if not isinstance(cases, list) or not 1 <= len(cases) <= 20:
            raise ValueError("test case count is invalid")
        results = []
        for case in cases:
            if not isinstance(case, dict) or set(case) != {"id", "input"} or not isinstance(case["id"], str):
                raise ValueError("test case shape is invalid")
            data = _series(case["input"])
            results.append({"id": case["id"], "output": _result(function(data), data)})
        output = {"results": results}
    else:
        data = _series(_read(Path(args.input), 96 * 1024))
        output = _result(function(data), data)
    encoded = json.dumps(output, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(encoded) > 64 * 1024:
        raise ValueError("extension result exceeds 64 KiB")
    Path(args.output).write_bytes(encoded)


if __name__ == "__main__":
    try:
        main()
    except BaseException as error:
        # No inputs, environment values, source text, or provider data go to stderr.
        print(f"extension child failed ({type(error).__name__})", file=sys.stderr)
        raise SystemExit(2)
