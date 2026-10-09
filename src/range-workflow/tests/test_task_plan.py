"""Adversarial checks of host-authoritative WF08 plan normalization."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.task_plan import PlanError, validate_plan


def catalog():
    return {
        "workflow.configure": {"kind": "command", "parameters": {
            "type": "object", "properties": {"damping": {"type": "number", "minimum": 0, "maximum": 1}},
            "required": ["damping"], "additionalProperties": False},
            "resources": [{"key": "configuration", "access": "write"}],
            "outputs": {"damping": {"type": "number", "unit": "1"}},
            "successPredicates": ["configure.matches"]},
        "workflow.run": {"kind": "command", "parameters": {"type": "object", "properties": {}},
            "resources": [{"key": "configuration", "access": "read"}],
            "outputs": {"error": {"type": "number", "unit": "m"}},
            "successPredicates": ["native.run.verified"]},
        "jobs.query": {"kind": "query", "parameters": {"type": "object", "properties": {}},
            "resources": [], "outputs": {"error": {"type": "number", "unit": "m"}},
            "successPredicates": ["query.identity"]}}


def candidate():
    return {"schema": "range.task-plan/v1", "title": "two points", "nodes": [
        {"id": "configure", "kind": "command", "tool": "workflow.configure",
         "parameters": {"damping": .05}, "successPredicate": "configure.matches"},
        {"id": "run", "kind": "command", "tool": "workflow.run",
         "dependsOn": ["configure"], "successPredicate": "native.run.verified"},
        {"id": "check", "kind": "condition", "dependsOn": ["run"],
         "condition": {"ref": {"node": "run", "output": "error"},
                       "operator": "le", "value": 1e-8, "unit": "m"}}]}


class TaskPlanTests(unittest.TestCase):
    def test_normalization_is_copied_and_resource_authoritative(self):
        raw, host = candidate(), catalog()
        normalized = validate_plan(raw, host)
        self.assertEqual(normalized["nodes"][0]["resources"], host["workflow.configure"]["resources"])
        normalized["nodes"][0]["resources"][0]["key"] = "other"
        self.assertEqual(host["workflow.configure"]["resources"][0]["key"], "configuration")
        self.assertNotIn("resources", raw["nodes"][0])

    def test_unknown_fields_tools_predicates_and_claims(self):
        for key, value in [("resources", []), ("tool", "python.eval"),
                           ("tool", []), ("successPredicate", []), ("kind", []),
                           ("successPredicate", "model.says.success"), ("kind", "query")]:
            with self.subTest(key=key):
                raw = candidate()
                raw["nodes"][0][key] = value
                with self.assertRaises(PlanError):
                    validate_plan(raw, catalog())

    def test_bool_nonfinite_and_bounds_are_not_numbers(self):
        for value in [True, float("nan"), float("inf"), -1, 2]:
            raw = candidate()
            raw["nodes"][0]["parameters"]["damping"] = value
            with self.subTest(value=value), self.assertRaises(PlanError):
                validate_plan(raw, catalog())

    def test_cycles_missing_and_unordered_writes(self):
        for dependency in ["check", "missing"]:
            raw = candidate()
            raw["nodes"][0]["dependsOn"] = [dependency]
            with self.assertRaises(PlanError):
                validate_plan(raw, catalog())
        raw = candidate()
        raw["nodes"][1]["dependsOn"] = []
        with self.assertRaises(PlanError):
            validate_plan(raw, catalog())

    def test_typed_ancestor_refs(self):
        raw = candidate()
        raw["nodes"].append({"id": "again", "kind": "command", "tool": "workflow.configure",
                             "dependsOn": ["check"], "inputRefs": {"damping": {"node": "configure", "output": "damping"}},
                             "successPredicate": "configure.matches"})
        validate_plan(raw, catalog())
        for ref in [{"node": "again", "output": "damping"}, {"node": "configure", "output": "hidden"}]:
            changed = copy.deepcopy(raw)
            changed["nodes"][-1]["inputRefs"]["damping"] = ref
            with self.assertRaises(PlanError):
                validate_plan(changed, catalog())

    def test_condition_and_clarification_refs_must_be_objects(self):
        for kind, malformed in (("condition", []), ("condition", None),
                                ("clarification", []), ("clarification", None)):
            with self.subTest(kind=kind, inputRefs=malformed):
                raw = candidate()
                if kind == "condition":
                    raw["nodes"][-1]["inputRefs"] = malformed
                else:
                    raw["nodes"].append({"id": "clarify", "kind": "clarification",
                                         "dependsOn": ["check"], "question": "Which case?",
                                         "inputRefs": malformed})
                with self.assertRaises(PlanError):
                    validate_plan(raw, catalog())

    def test_conditions_are_finite_unit_bound_and_closed(self):
        for key, value in [("unit", "mm"), ("operator", "eval"), ("value", True), ("expression", "1==1")]:
            raw = candidate()
            raw["nodes"][-1]["condition"][key] = value
            with self.subTest(key=key), self.assertRaises(PlanError):
                validate_plan(raw, catalog())

    def test_limits(self):
        for key, value in [("toolCalls", 129), ("modelCalls", True), ("readOnlyParallel", 3), ("activeSeconds", 1801), ("deadlineSeconds", 86401)]:
            raw = candidate()
            raw["budgets"] = {key: value}
            with self.assertRaises(PlanError):
                validate_plan(raw, catalog())
        raw = candidate()
        raw["nodes"][0]["budgets"] = {"modelCalls": 5}
        with self.assertRaises(PlanError):
            validate_plan(raw, catalog())
        raw = candidate()
        raw["nodes"] = raw["nodes"] * 22
        with self.assertRaises(PlanError):
            validate_plan(raw, catalog())
        raw = candidate()
        raw["title"] = "x" * 257
        with self.assertRaises(PlanError):
            validate_plan(raw, catalog())


if __name__ == "__main__":
    unittest.main()
