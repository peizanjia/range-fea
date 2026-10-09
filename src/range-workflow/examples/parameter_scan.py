"""Bounded registered-source scan. Every command requires a host user callback.

The callback receives the full immutable plan and pending node. Returning True
must mean an actual user approval; callers must never substitute an LLM answer.
This module performs no work on import and grants no approval by default.
"""
from __future__ import annotations

import time


def scan_plan(points=(0.05, 0.10), *, dt=0.005, steps=20):
    points = tuple(points)
    if not 1 <= len(points) <= 4:
        raise ValueError("A scan requires one to four fixed configuration points")
    nodes, previous = [], []
    for index, damping in enumerate(points):
        configure, run, check = (f"{name}{index}" for name in ("configure", "run", "check"))
        nodes.extend([
            {"id": configure, "kind": "command", "tool": "workflow.configure",
             "dependsOn": previous, "parameters": {"dt": dt, "steps": steps, "targetDamping": damping},
             "successPredicate": "configure.matches"},
            {"id": run, "kind": "command", "tool": "workflow.run", "dependsOn": [configure],
             "parameters": {"mode": "both", "independentReference": True},
             "successPredicate": "native.run.verified"},
            {"id": check, "kind": "condition", "dependsOn": [run],
             "condition": {"ref": {"node": run, "output": "maxDisplacementErrorM"},
                           "operator": "le", "value": 1e-8, "unit": "m"}}])
        previous = [check]
    return {"schema": "range.task-plan/v1", "title": "Registered native damping scan",
            "budgets": {"modelCalls": 0, "toolCalls": 64, "deadlineSeconds": 1800}, "nodes": nodes}


def run_scan(sdk, approve_node, *, plan=None, timeout=600):
    """Current configuration must already contain a registered native sourceId.

    A rejected approval pauses the plan. Timeout also pauses dispatch and raises;
    existing jobs remain observable through their accepted receipts.
    """
    if not callable(approve_node):
        raise TypeError("An explicit user approval callback is required")
    staged = sdk.request("task.plan.propose", {"plan": plan or scan_plan()})
    controls = {"planId": staged["planId"], "expectedPlanSha256": staged["planSha256"]}
    sdk.request("task.plan.start", controls)
    deadline = time.monotonic() + timeout
    approved_previews = set()
    while time.monotonic() < deadline:
        observed = sdk.request("task.plan.query", {"planId": staged["planId"]})
        if observed["state"] in {"completed", "failed", "conflict", "cancelled", "superseded", "paused"}:
            return observed
        for node in observed["nodes"]:
            if node["state"] != "awaiting_approval":
                continue
            approval_key = (node["id"], node["previewHash"])
            if approval_key in approved_previews:
                continue
            if approve_node(observed, node) is not True:
                return sdk.request("task.plan.pause", controls)
            sdk.request("host.task.node.approve", {**controls, "nodeId": node["id"],
                                                  "previewHash": node["previewHash"]})
            approved_previews.add(approval_key)
        time.sleep(0.25)
    sdk.request("task.plan.pause", controls)
    raise TimeoutError("Scan paused after the host observation deadline")
