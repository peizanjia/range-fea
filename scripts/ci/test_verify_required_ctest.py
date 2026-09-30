import json
import subprocess
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

from verify_required_ctest import (
    CTestCommandError,
    find_disabled_required_tests,
    find_missing_tests,
    parse_ctest_inventory,
    parse_ctest_names,
    parse_required_test_groups,
    run_ctest_inventory,
)


class RequiredCTestInventoryTests(unittest.TestCase):
    def test_required_groups_pass_when_all_names_are_registered_and_allow_extras(self) -> None:
        groups = parse_required_test_groups(
            json.dumps(
                {
                    "schema_version": 1,
                    "required_test_groups": {"native": ["tst_vector"], "python": ["tst_numeric"]},
                }
            )
        )
        registered = parse_ctest_names(
            json.dumps({"tests": [{"name": "tst_vector"}, {"name": "tst_numeric"}, {"name": "tst_future"}]})
        )

        self.assertEqual(find_missing_tests(registered, groups), {})

    def test_missing_required_test_is_reported_in_its_group(self) -> None:
        groups = {"solver": ("tst_solver", "tst_numeric")}

        self.assertEqual(
            find_missing_tests({"tst_solver"}, groups),
            {"solver": ["tst_numeric"]},
        )

    def test_disabled_required_test_does_not_satisfy_manifest(self) -> None:
        groups = {"native": ("tst_required",)}
        registered, disabled = parse_ctest_inventory(
            json.dumps(
                {
                    "tests": [
                        {
                            "name": "tst_required",
                            "properties": [{"name": "DISABLED", "value": True}],
                        },
                        {
                            "name": "tst_optional",
                            "properties": [{"name": "DISABLED", "value": True}],
                        },
                    ]
                }
            )
        )

        self.assertEqual(find_missing_tests(registered, groups), {})
        self.assertEqual(find_disabled_required_tests(disabled, groups), {"native": ["tst_required"]})

    def test_malformed_manifest_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "not valid JSON"):
            parse_required_test_groups("{")

    def test_empty_manifest_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "no required_test_groups"):
            parse_required_test_groups(json.dumps({"schema_version": 1, "required_test_groups": {}}))

    def test_nonzero_ctest_inventory_process_is_reported(self) -> None:
        runner = Mock(return_value=SimpleNamespace(returncode=7, stdout="partial", stderr="ctest failed"))

        with self.assertRaisesRegex(CTestCommandError, "status 7") as raised:
            run_ctest_inventory(Path("build"), "Release", runner=runner)

        self.assertEqual(raised.exception.stdout, "partial")
        self.assertEqual(raised.exception.stderr, "ctest failed")

    def test_missing_ctest_executable_is_reported(self) -> None:
        runner = Mock(side_effect=FileNotFoundError("ctest not found"))

        with self.assertRaisesRegex(CTestCommandError, "could not run CTest"):
            run_ctest_inventory(Path("build"), None, runner=runner)


if __name__ == "__main__":
    unittest.main()
