#!/usr/bin/env python3
"""Fail when the configured CTest inventory omits a required named test."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Callable, Sequence


class CTestCommandError(RuntimeError):
    """The CTest inventory command could not be completed successfully."""

    def __init__(self, message: str, stdout: str = "", stderr: str = "") -> None:
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr


def parse_required_test_groups(text: str) -> dict[str, tuple[str, ...]]:
    """Load and validate the required-test manifest."""
    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"required-test manifest is not valid JSON: {exc}") from exc

    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        raise ValueError("required-test manifest must be an object with schema_version 1")

    groups = manifest.get("required_test_groups")
    if not isinstance(groups, dict) or not groups:
        raise ValueError("required-test manifest has no required_test_groups")

    result: dict[str, tuple[str, ...]] = {}
    seen: set[str] = set()
    for group, tests in groups.items():
        if not isinstance(group, str) or not group.strip():
            raise ValueError("required-test group names must be non-empty strings")
        if not isinstance(tests, list) or not tests:
            raise ValueError(f"required-test group {group!r} must contain at least one test name")

        names: list[str] = []
        for name in tests:
            if not isinstance(name, str) or not name.strip():
                raise ValueError(f"required-test group {group!r} contains an empty or invalid test name")
            if name in seen:
                raise ValueError(f"required test {name!r} appears in more than one manifest group")
            names.append(name)
            seen.add(name)
        result[group] = tuple(names)

    return result


def parse_ctest_inventory(text: str) -> tuple[set[str], set[str]]:
    """Extract unique test names and tests marked DISABLED from CTest JSON."""
    try:
        inventory = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"CTest inventory is not valid JSON: {exc}") from exc

    tests = inventory.get("tests") if isinstance(inventory, dict) else None
    if not isinstance(tests, list) or not tests:
        raise ValueError("CTest inventory has no registered tests")

    names: set[str] = set()
    disabled_names: set[str] = set()
    for index, test in enumerate(tests):
        name = test.get("name") if isinstance(test, dict) else None
        if not isinstance(name, str) or not name.strip():
            raise ValueError(f"CTest inventory test at index {index} has no valid name")
        if name in names:
            raise ValueError(f"CTest inventory contains duplicate test name {name!r}")
        names.add(name)

        properties = test.get("properties", [])
        if not isinstance(properties, list):
            raise ValueError(f"CTest inventory properties for {name!r} are not a list")
        for prop in properties:
            if not isinstance(prop, dict):
                raise ValueError(f"CTest inventory has an invalid property for {name!r}")
            property_name = prop.get("name")
            if not isinstance(property_name, str) or not property_name:
                raise ValueError(f"CTest inventory has an unnamed property for {name!r}")
            if property_name != "DISABLED":
                continue
            value = prop.get("value")
            if isinstance(value, bool):
                is_disabled = value
            elif isinstance(value, str) and value.casefold() in {"true", "1", "on", "yes"}:
                is_disabled = True
            elif isinstance(value, str) and value.casefold() in {"false", "0", "off", "no"}:
                is_disabled = False
            else:
                raise ValueError(f"CTest DISABLED property for {name!r} has an invalid value")
            if is_disabled:
                disabled_names.add(name)

    return names, disabled_names


def parse_ctest_names(text: str) -> set[str]:
    """Extract unique named tests from CTest's --show-only=json-v1 output."""
    return parse_ctest_inventory(text)[0]


def find_missing_tests(
    registered_tests: set[str], required_groups: dict[str, tuple[str, ...]]
) -> dict[str, list[str]]:
    """Return required tests absent from the inventory, grouped by manifest group."""
    missing = {
        group: [name for name in required if name not in registered_tests]
        for group, required in required_groups.items()
    }
    return {group: tests for group, tests in missing.items() if tests}


def find_disabled_required_tests(
    disabled_tests: set[str], required_groups: dict[str, tuple[str, ...]]
) -> dict[str, list[str]]:
    """Return required tests that are registered but disabled in CTest."""
    disabled = {
        group: [name for name in required if name in disabled_tests]
        for group, required in required_groups.items()
    }
    return {group: tests for group, tests in disabled.items() if tests}


def run_ctest_inventory(
    build_dir: Path,
    configuration: str | None,
    ctest_executable: str = "ctest",
    runner: Callable[..., subprocess.CompletedProcess[str]] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run CTest's inventory query and preserve command failures for diagnostics."""
    command = [ctest_executable, "--test-dir", str(build_dir)]
    if configuration:
        command.extend(["-C", configuration])
    command.append("--show-only=json-v1")

    if runner is None:
        runner = subprocess.run
    try:
        result = runner(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as exc:
        raise CTestCommandError(f"could not run CTest inventory command: {exc}") from exc

    if result.returncode != 0:
        raise CTestCommandError(
            f"CTest inventory command exited with status {result.returncode}",
            stdout=result.stdout,
            stderr=result.stderr,
        )
    return result


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True, help="CMake build directory")
    parser.add_argument("--configuration", help="multi-config build configuration, such as Release")
    parser.add_argument("--manifest", type=Path, required=True, help="required CTest names JSON file")
    parser.add_argument(
        "--inventory-out",
        type=Path,
        required=True,
        help="path for raw ctest --show-only=json-v1 output",
    )
    parser.add_argument("--ctest", default="ctest", help="CTest executable to invoke")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)

    try:
        manifest_text = args.manifest.read_text(encoding="utf-8")
        required_groups = parse_required_test_groups(manifest_text)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    try:
        result = run_ctest_inventory(args.build_dir, args.configuration, args.ctest)
    except CTestCommandError as exc:
        args.inventory_out.parent.mkdir(parents=True, exist_ok=True)
        args.inventory_out.write_text(exc.stdout, encoding="utf-8")
        print(f"error: {exc}", file=sys.stderr)
        if exc.stderr:
            print(exc.stderr.rstrip(), file=sys.stderr)
        return 1

    try:
        args.inventory_out.parent.mkdir(parents=True, exist_ok=True)
        args.inventory_out.write_text(result.stdout, encoding="utf-8")
        registered_tests, disabled_tests = parse_ctest_inventory(result.stdout)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    missing = find_missing_tests(registered_tests, required_groups)
    disabled = find_disabled_required_tests(disabled_tests, required_groups)
    if missing:
        for group, names in missing.items():
            print(f"Missing required tests in {group}:", file=sys.stderr)
            for name in names:
                print(f"  {name}", file=sys.stderr)
    if disabled:
        for group, names in disabled.items():
            print(f"Required tests are disabled in {group}:", file=sys.stderr)
            for name in names:
                print(f"  {name}", file=sys.stderr)
    if missing or disabled:
        return 1

    required_count = sum(len(names) for names in required_groups.values())
    print(
        f"Verified {required_count} required tests across {len(required_groups)} groups; "
        f"CTest registered {len(registered_tests)} tests."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
