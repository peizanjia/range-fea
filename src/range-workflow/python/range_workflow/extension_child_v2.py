"""Trusted developer v2 file protocol. Isolated Python is not an OS sandbox.

Run a fixed argv: python -I extension_child_v2.py --source managed.py
--input managed-input.json --output managed-output.json --run
Paths and trusted developer permission are owned by the host, never input JSON.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import types

# Load only the neighbouring contract; do not import lifecycle/package init.
_contract_path = Path(__file__).with_name("scientific_contract.py")
_contract = types.ModuleType("range_scientific_contract")
_contract.__file__ = str(_contract_path)
exec(compile(_contract_path.read_bytes(), str(_contract_path), "exec"), _contract.__dict__)


def _read(path):
    with Path(path).open("rb") as stream:
        return _contract.strict_json(stream.read(_contract.MAX_JSON_BYTES + 1))


def main(argv=None):
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--source", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--run", action="store_true")
    modes.add_argument("--test", action="store_true")
    args = parser.parse_args(argv)
    if not sys.flags.isolated:
        raise ValueError("v2 requires isolated interpreter")
    import numpy
    import scipy
    if {"numpy": numpy.__version__, "scipy": scipy.__version__} != _contract.DEPENDENCIES:
        raise ValueError("scientific dependency versions differ from frozen environment")
    source = Path(args.source)
    if not source.is_file() or source.stat().st_size > 64*1024:
        raise ValueError("invalid source size")
    module = types.ModuleType("range_approved_extension_v2")
    module.__file__ = str(source)
    source_bytes = source.read_bytes()
    if len(source_bytes) > 64*1024:
        raise ValueError("invalid source size")
    exec(compile(source_bytes, str(source), "exec"), module.__dict__)
    process = getattr(module, "process", None)
    if not callable(process):
        raise ValueError("extension must export process(data)")
    value = _read(args.input)
    def execute(data):
        normalized = _contract.validate_input(data)
        # Keep validated baseline separate from generated code mutation.
        baseline = json.loads(json.dumps(normalized, allow_nan=False))
        return _contract.validate_output(process(normalized), baseline)
    if args.test:
        _contract.exact(value, {"cases"})
        if type(value["cases"]) is not list or not 1 <= len(value["cases"]) <= 16:
            raise ValueError("invalid test case count")
        results, ids = [], set()
        for case in value["cases"]:
            _contract.exact(case, {"id", "input"})
            ident = _contract._id(case["id"])
            if ident in ids:
                raise ValueError("duplicate case identifier")
            ids.add(ident)
            results.append({"id": ident, "output": execute(case["input"])})
        output = {"results": results}
    else:
        output = execute(value)
    encoded = json.dumps(output, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(encoded) > _contract.MAX_JSON_BYTES:
        raise ValueError("result exceeds 32 MiB")
    Path(args.output).write_bytes(encoded)


if __name__ == "__main__":
    try:
        main()
    except BaseException as error:
        print(f"extension v2 child failed ({type(error).__name__})", file=sys.stderr)
        raise SystemExit(2)
