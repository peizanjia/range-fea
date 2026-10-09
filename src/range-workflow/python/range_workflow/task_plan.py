"""Closed, host-catalogue-driven task plan validation. No execution or I/O."""
from __future__ import annotations

import copy
import hashlib
import json
import math
import re

MAX_BYTES = 128 * 1024
LIMITS = {"modelCalls": 24, "toolCalls": 128, "readOnlyParallel": 2,
          "activeSeconds": 1800, "deadlineSeconds": 86400}
NODE_LIMITS = {"modelCalls": 4, "toolCalls": 16, "activeSeconds": 300}
PREDICATES = {"configure.matches", "native.run.verified", "query.identity",
              "condition.matches", "clarification.received"}
TOOLS = {"workflow.configure", "workflow.run", "jobs.query", "results.query",
         "result.summary.query"}
ID = re.compile(r"[A-Za-z][A-Za-z0-9_-]{0,63}\Z")


class PlanError(ValueError):
    """The candidate cannot be safely normalized."""


def plan_schema():
    """Machine discovery for the candidate format; host catalog adds tool schemas."""
    def closed(properties, required=()):
        return {"type": "object", "properties": properties, "required": list(required), "additionalProperties": False}
    ident = {"type": "string", "pattern": ID.pattern, "maxLength": 64}
    ref = closed({"node": ident, "output": {"type": "string", "maxLength": 128}}, ("node", "output"))
    budgets = lambda limits: closed({key: {"type": "integer", "minimum": 0, "maximum": value}
                                    for key, value in limits.items()})
    node = closed({"id": ident, "kind": {"type": "string", "enum": ["command", "query", "condition", "clarification"]},
        "tool": {"type": "string", "enum": sorted(TOOLS)}, "parameters": {"type": "object"},
        "dependsOn": {"type": "array", "items": ident, "maxItems": 16, "uniqueItems": True},
        "inputRefs": {"type": "object", "additionalProperties": ref},
        "successPredicate": {"type": "string", "enum": sorted(PREDICATES)},
        "failurePolicy": {"type": "string", "enum": ["stop", "continue-independent"]},
        "condition": closed({"ref": ref, "operator": {"type": "string", "enum": ["eq", "lt", "le", "gt", "ge"]},
            "value": {"type": "number"}, "unit": {"type": "string"}}, ("ref", "operator", "value", "unit")),
        "question": {"type": "string", "minLength": 1, "maxLength": 4096}, "budgets": budgets(NODE_LIMITS)}, ("id", "kind"))
    result = closed({"schema": {"type": "string", "const": "range.task-plan/v1"},
        "title": {"type": "string", "maxLength": 256},
        "nodes": {"type": "array", "items": node, "minItems": 1, "maxItems": 64},
        "budgets": budgets(LIMITS)}, ("schema", "title", "nodes"))
    result["description"] = "Kind-specific fields, ancestor output types, resources and predicates are further checked against the host-owned task tool catalog. Parameters cannot include code or authority."
    return result


def canonical(value):
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=True, allow_nan=False)
    except (ValueError, TypeError, RecursionError) as exc:
        raise PlanError("expected finite JSON") from exc


def digest(value):
    return hashlib.sha256(canonical(value).encode("ascii")).hexdigest()


def _closed(value, allowed, required=()):
    if not isinstance(value, dict) or set(value) - set(allowed) or set(required) - set(value):
        raise PlanError("unknown or missing object fields")


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _schema(value, schema, depth=0):
    """Small explicit JSON Schema subset; unknown host keywords fail closed."""
    if depth > 16:
        raise PlanError("parameter nesting limit")
    _closed(schema, {"type", "properties", "required", "additionalProperties", "items",
                     "enum", "minimum", "maximum", "minLength", "maxLength", "minItems",
                     "maxItems", "default", "description", "title", "pattern", "const",
                     "exclusiveMinimum", "exclusiveMaximum"}, {"type"})
    kind = schema["type"]
    if type(kind) is not str:
        raise PlanError("unsupported host schema type")
    valid = {"object": isinstance(value, dict), "array": isinstance(value, list),
             "string": isinstance(value, str), "integer": type(value) is int,
             "number": _number(value), "boolean": type(value) is bool,
             "null": value is None}
    if kind not in valid or not valid[kind]:
        raise PlanError("parameter type mismatch")
    if "enum" in schema and not any(type(value) is type(item) and value == item for item in schema["enum"]):
        raise PlanError("parameter outside enum")
    if "const" in schema and (type(value) is not type(schema["const"]) or value != schema["const"]):
        raise PlanError("parameter differs from required constant")
    if kind == "object":
        props = schema.get("properties", {})
        if schema.get("additionalProperties", False) is not False:
            raise PlanError("tool parameter objects must be closed")
        if set(value) - set(props) or set(schema.get("required", [])) - set(value):
            raise PlanError("unknown or missing parameter")
        return {key: _schema(value[key], props[key], depth + 1) for key in sorted(value)}
    if kind == "array":
        if not schema.get("minItems", 0) <= len(value) <= schema.get("maxItems", 256):
            raise PlanError("array bounds")
        return [_schema(item, schema["items"], depth + 1) for item in value]
    if kind == "string" and not schema.get("minLength", 0) <= len(value) <= schema.get("maxLength", 4096):
        raise PlanError("string bounds")
    if kind == "string" and "pattern" in schema and re.fullmatch(schema["pattern"], value) is None:
        raise PlanError("string pattern")
    if kind in {"number", "integer"} and not schema.get("minimum", -math.inf) <= value <= schema.get("maximum", math.inf):
        raise PlanError("number bounds")
    if kind in {"number", "integer"} and (
            ("exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"])
            or ("exclusiveMaximum" in schema and value >= schema["exclusiveMaximum"])):
        raise PlanError("exclusive number bounds")
    return copy.deepcopy(value)


def _budgets(value, limits):
    _closed(value, limits)
    result = dict(limits)
    for key, number in value.items():
        if type(number) is not int or not 0 <= number <= limits[key]:
            raise PlanError("budget exceeds host limit")
        result[key] = number
    return result


def validate_plan(candidate, catalog):
    """Return a fresh normalized plan; catalog is exclusively supplied by host.

    inputRefs map top-level parameter names to ancestor output {node, output}.
    Referenced values are resolved and schema-checked by the host before dispatch.
    """
    if len(canonical(candidate).encode("ascii")) > MAX_BYTES:
        raise PlanError("plan exceeds byte limit")
    _closed(candidate, {"schema", "title", "nodes", "budgets"}, {"schema", "title", "nodes"})
    if candidate["schema"] != "range.task-plan/v1" or not isinstance(candidate["title"], str) or len(candidate["title"]) > 256:
        raise PlanError("invalid schema or title")
    raw = candidate["nodes"]
    if not isinstance(raw, list) or not 1 <= len(raw) <= 64:
        raise PlanError("node count")
    nodes = []
    by_id = {}
    for item in raw:
        _closed(item, {"id", "kind", "tool", "parameters", "dependsOn", "inputRefs",
                       "successPredicate", "failurePolicy", "condition", "question", "budgets"}, {"id", "kind"})
        ident = item["id"]
        if not isinstance(ident, str) or not ID.fullmatch(ident) or ident in by_id:
            raise PlanError("invalid or duplicate node id")
        kind = item["kind"]
        if not isinstance(kind, str):
            raise PlanError("invalid node kind")
        deps = item.get("dependsOn", [])
        if not isinstance(deps, list) or len(deps) > 16 or any(not isinstance(d, str) for d in deps) or len(set(deps)) != len(deps):
            raise PlanError("invalid dependencies")
        node = {"id": ident, "kind": kind, "dependsOn": list(deps), "parameters": {},
                "inputRefs": copy.deepcopy(item.get("inputRefs", {})), "resources": [],
                "failurePolicy": item.get("failurePolicy", "stop"),
                "budgets": _budgets(item.get("budgets", {}), NODE_LIMITS)}
        if type(node["inputRefs"]) is not dict:
            raise PlanError("input references must be an object")
        if not isinstance(node["failurePolicy"], str) or node["failurePolicy"] not in {"stop", "continue-independent"}:
            raise PlanError("failure policy")
        if kind in {"command", "query"}:
            tool = item.get("tool")
            if not isinstance(tool, str) or tool not in TOOLS or tool not in catalog:
                raise PlanError("unknown tool")
            entry = catalog[tool]
            if entry["kind"] != kind:
                raise PlanError("read-only claim escalation")
            refs = node["inputRefs"]
            if not isinstance(refs, dict):
                raise PlanError("invalid refs")
            parameters = item.get("parameters", {})
            schema = copy.deepcopy(entry["parameters"])
            if not isinstance(parameters, dict) or set(refs) & set(parameters):
                raise PlanError("duplicate parameter source")
            schema["required"] = [key for key in schema.get("required", []) if key not in refs]
            node["parameters"] = _schema(parameters, schema)
            node["tool"] = tool
            node["resources"] = copy.deepcopy(entry.get("resources", []))
            for resource in node["resources"]:
                _closed(resource, {"key", "access"}, {"key", "access"})
                if not isinstance(resource["key"], str) or not resource["key"] or resource["access"] not in {"read", "write"}:
                    raise PlanError("invalid host resource")
            if kind == "query" and any(r["access"] == "write" for r in node["resources"]):
                raise PlanError("read-only claim escalation")
            predicate = item.get("successPredicate")
            if not isinstance(predicate, str) or predicate not in PREDICATES or predicate not in entry["successPredicates"]:
                raise PlanError("unknown success predicate")
            if "condition" in item or "question" in item:
                raise PlanError("unexpected control fields")
        elif kind == "condition":
            if any(key in item for key in ("tool", "parameters", "question")):
                raise PlanError("unexpected condition fields")
            condition = item.get("condition")
            _closed(condition, {"ref", "operator", "value", "unit"}, {"ref", "operator", "value", "unit"})
            if not isinstance(condition["operator"], str) or condition["operator"] not in {"eq", "lt", "le", "gt", "ge"} or not _number(condition["value"]) or not isinstance(condition["unit"], str):
                raise PlanError("invalid scalar condition")
            node["condition"] = copy.deepcopy(condition)
            predicate = item.get("successPredicate", "condition.matches")
            if predicate != "condition.matches" or node["inputRefs"]:
                raise PlanError("condition predicate or refs")
        elif kind == "clarification":
            if any(key in item for key in ("tool", "parameters", "condition")) or node["inputRefs"]:
                raise PlanError("unexpected clarification fields")
            question = item.get("question")
            if not isinstance(question, str) or not 1 <= len(question) <= 4096:
                raise PlanError("invalid question")
            node["question"] = question
            predicate = item.get("successPredicate", "clarification.received")
            if predicate != "clarification.received":
                raise PlanError("clarification predicate")
        else:
            raise PlanError("unknown node kind")
        node["successPredicate"] = predicate
        nodes.append(node)
        by_id[ident] = node
    ancestors = {}
    def visit(ident, path):
        if ident not in by_id or ident in path:
            raise PlanError("missing dependency or cycle")
        if ident not in ancestors:
            result = set()
            for dep in by_id[ident]["dependsOn"]:
                result.add(dep)
                result.update(visit(dep, path | {ident}))
            ancestors[ident] = result
        return ancestors[ident]
    def output_ref(ref, node):
        _closed(ref, {"node", "output"}, {"node", "output"})
        source, output = ref["node"], ref["output"]
        if not isinstance(source, str) or not isinstance(output, str) or source not in ancestors[node["id"]]:
            raise PlanError("reference is not an ancestor")
        owner = by_id[source]
        spec = catalog.get(owner.get("tool"), {}).get("outputs", {}).get(output)
        if not isinstance(spec, dict) or spec.get("type") not in {"number", "integer", "string", "boolean"}:
            raise PlanError("output is not a typed whitelist entry")
        return spec
    for node in nodes:
        visit(node["id"], set())
    for node in nodes:
        for parameter, ref in node["inputRefs"].items():
            spec = output_ref(ref, node)
            expected = catalog[node["tool"]]["parameters"].get("properties", {}).get(parameter)
            if not expected or spec["type"] != expected["type"]:
                raise PlanError("reference type mismatch")
        if node["kind"] == "condition":
            spec = output_ref(node["condition"]["ref"], node)
            if spec["type"] not in {"number", "integer"} or spec.get("unit") != node["condition"]["unit"]:
                raise PlanError("condition unit or scalar mismatch")
    for index, left in enumerate(nodes):
        for right in nodes[index + 1:]:
            if left["id"] in ancestors[right["id"]] or right["id"] in ancestors[left["id"]]:
                continue
            if any(a["key"] == b["key"] and "write" in (a["access"], b["access"])
                   for a in left["resources"] for b in right["resources"]):
                raise PlanError("unordered resource write conflict")
    return {"schema": candidate["schema"], "title": candidate["title"], "nodes": nodes,
            "budgets": _budgets(candidate.get("budgets", {}), LIMITS)}
