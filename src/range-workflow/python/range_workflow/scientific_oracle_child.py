"""Fixed trusted host oracle in the frozen private interpreter, never generated code."""
from pathlib import Path
import argparse
import json
import sys
import types


def main():
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--test", action="store_true")
    args = parser.parse_args()
    if not sys.flags.isolated:
        raise ValueError("isolated private oracle required")
    # A private package namespace contains exactly the two hashed source
    # modules. No package __init__, import loader or bytecode cache executes.
    package_name = "_range_verified_oracle"
    package = types.ModuleType(package_name)
    package.__path__ = []
    sys.modules[package_name] = package
    def source_module(name):
        path = Path(__file__).with_name(name + ".py")
        module = types.ModuleType(package_name + "." + name)
        module.__file__, module.__package__ = str(path), package_name
        sys.modules[module.__name__] = module
        exec(compile(path.read_bytes(), str(path), "exec"), module.__dict__)
        return module
    contract = source_module("scientific_contract")
    reference_result = source_module("scientific_reference").reference_result
    import numpy, scipy
    if {"numpy": numpy.__version__, "scipy": scipy.__version__} != contract.DEPENDENCIES:
        raise ValueError("oracle package version changed")
    with Path(args.input).open("rb") as stream:
        data = contract.strict_json(stream.read(contract.MAX_JSON_BYTES+1))
    if args.test:
        contract.exact(data, {"cases"})
        if type(data["cases"]) is not list or not 1 <= len(data["cases"]) <= 16:
            raise ValueError("bounded oracle batch required")
        output = {"results": [{"id": c["id"], "output": reference_result(c["input"], small_n=True)}
                              for c in data["cases"]]}
    else:
        output = reference_result(data, small_n=len(data["times"]) <= 128)
    encoded = json.dumps(output, sort_keys=True, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode()
    if len(encoded) > contract.MAX_JSON_BYTES:
        raise ValueError("oracle output limit")
    with Path(args.output).open("xb") as stream:
        stream.write(encoded)


if __name__ == "__main__":
    main()
