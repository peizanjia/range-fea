"""Isolated WF10 GUI protocol fixture. No FE or performance evidence.

Copied into a temporary range_workflow package by the Qt tests. All plots,
proofs and CSV bytes here are synthetic; only transport/UI behavior is tested.
"""
import base64
import copy
import hashlib
import json
import sys

JOBS = ["job-a", "job-b"]
revision = 0
defer = False
deferred = []
reverse = False
bad = ""
calls = []
exports = {}


def chosen(job="job-a", channel="node:51000:velocity:X", kind="native"):
    result = dict(kind=kind, jobId=job, mode="active", artifactId="artifact-" + job)
    if kind != "energy":
        result["channelId"] = channel
    if kind == "reference":
        result["referenceSha256"] = "c" * 64
    return result


def pin(s, rev=None):
    result = copy.deepcopy(s)
    result.pop("sourceHash", None)
    result["sourceHash"] = hashlib.sha256((json.dumps(result, sort_keys=True) + str(revision if rev is None else rev)).encode()).hexdigest()
    return result


def meta(s):
    s = pin(s)
    kind = s["kind"]
    channel = dict(channelId=s.get("channelId", "derived:mechanical-energy"),
                   variableId="velocity", quantity="velocity", axis="X", unit="m/s",
                   nodeTarget="persistent-node-physical-id", nodePhysicalId=51000)
    result = dict(selector=s, sourceKind="native-active-fe", sourceHash=s["sourceHash"],
                  modelSha256="a"*64, manifestSha256="b"*64, rawSampleCount=2000,
                  channel=channel, proofScope="solver-and-declared-controller-runtime/v1")
    if kind == "reference":
        result.update(sourceKind="independent-reference", referenceSha256="c"*64,
                      proofScope="independent-matrix-newmark-reference/v1")
    if kind == "energy":
        channel.update(channelId="derived:mechanical-energy", unit="J", axis="scalar")
        result.update(sourceKind="derived-mechanical-energy", derived=True,
                      derivation="0.5*qT*K*q+0.5*vT*M*v", physicalChannelIds=[],
                      underlyingSourceKind="native-active-fe", linearSha256="d"*64,
                       matrixSha256="e"*64, proofScope="derived-physical-dofs-independent-matrices/v1")
    if s.get("channelId") == "control:work":
        channel.update(variableId="control-cumulative-work", quantity="controlWork", axis="scalar",
                       unit="J", nodeTarget="accepted-control-ledger")
        channel.pop("nodePhysicalId", None)
    return result


def plot(p):
    view = p["view"]
    items = copy.deepcopy(p["sources"])
    if view == "energy":
        for item in items:
            item["kind"] = "energy"
            item.pop("channelId", None)
    if view == "work":
        for item in items:
            item["channelId"] = "control:work"
    metas = [meta(s) for s in items]
    unit = "J" if view in ("energy", "work") else "m/s"
    if view == "psd":
        unit = "(m/s)^2/Hz"
    spectrum = view in ("amplitude", "psd")
    traces = []
    groups = [metas] if view in ("error", "phase") else [[m] for m in metas]
    for i, group in enumerate(groups):
        points = [dict(x=j * .01, y=j + revision, ordinal=j) for j in (0, 10, 100, 999)]
        if spectrum:
            scale = 2 if p["options"]["window"] == "hann" else 3
            for point in points:
                point["y"] = point["y"] * scale + (0 if p["options"]["removeMean"] else 7) + (100 if view == "psd" else 0)
        trace = dict(id="trace-" + str(i+1), label="Synthetic trace", unit=unit,
                     sources=group, points=points, rawSampleCount=2000,
                     displaySampleCount=len(points), displayOnly=True)
        if spectrum:
            trace.update(spectralBinCount=1001, window=p["options"]["window"], removeMean=p["options"]["removeMean"])
        traces.append(trace)
    result = dict(schemaId="range.series-plot/v1", methodVersion="range.series-analysis/v1",
                  view=view, title="Synthetic " + view,
                  axes=dict(x=dict(label="Frequency" if spectrum else "Time", unit="Hz" if spectrum else "m/s" if view == "phase" else "s"),
                            y=dict(label=view, unit=unit)), traces=traces, unavailable=[],
                  export=dict(available=True, scope="complete-raw", sources=[m["selector"] for m in metas]))
    if bad == "unit":
        result["traces"][0]["unit"] = "N"
    elif bad == "reference":
        result["traces"][0]["sources"][0]["referenceSha256"] = "f" * 64
    elif bad == "ordinal":
        result["traces"][0]["points"][0]["ordinal"] = 2000
    elif bad == "unknown":
        result["arbitraryPath"] = "do-not-accept"
    elif bad in ("work-job", "work-mode", "work-artifact"):
        key = {"work-job": "jobId", "work-mode": "mode", "work-artifact": "artifactId"}[bad]
        result["traces"][0]["sources"][0]["selector"][key] = "passive" if key == "mode" else "wrong-owner"
    return result


def respond(r, result, code="OK"):
    print(json.dumps(dict(apiVersion="range.workflow/v1", requestId=r["requestId"],
                         status="ok" if code == "OK" else "error", code=code, result=result)), flush=True)


def dispatch(r):
    global revision, defer, reverse, bad
    op, p = r["operation"], r.get("parameters", {})
    calls.append(dict(operation=op, parameters=copy.deepcopy(p)))
    if op == "capabilities.query":
        return dict(apiVersion="range.workflow/v1")
    if op == "project.query":
        return dict(projectId="fixture-project")
    if op == "jobs.query":
        return dict(jobs=[dict(jobId=j, state="succeeded", command="workflow.run") for j in (list(reversed(JOBS)) if reverse else JOBS)])
    if op == "result.plot.sources.query":
        job = p["jobId"]
        rows = []
        for axis in ("X", "Y", "Z"):
            s = chosen(job, "node:51000:velocity:" + axis)
            s["mode"] = p["mode"]
            row = dict(selector=s, label="Velocity " + axis, unit="m/s", quantity="velocity")
            if axis == "X" and p["mode"] == "active":
                row["reference"] = chosen(job, kind="reference")
            rows.append(row)
        if reverse:
            rows.reverse()
        energy = chosen(job, kind="energy"); energy["mode"] = p["mode"]
        return dict(schemaId="range.series-source-catalog/v1", jobId=job, mode=p["mode"],
                    artifactId="artifact-" + job, channels=rows,
                    derived=[dict(selector=energy, label="Mechanical energy", unit="J")], unavailable=[])
    if op == "result.plot.query":
        if any("sourceHash" in s and s["sourceHash"] != pin(s)["sourceHash"] for s in p["sources"]):
            respond(r, dict(message="Saved source hash changed"), "SOURCE_CHANGED")
            return None
        result = plot(p)
        if defer:
            deferred.append((r, result))
            return None
        return result
    if op == "result.raw.export":
        # A held plot is exportable at its original identity in this fixture.
        rows = ["source,ordinal,timeS,value,sourceHash\n"]
        for i, s in enumerate(p["sources"]):
            for n in range(2000):
                rows.append(f"{i},{n},{n*.01},{n},{s['sourceHash']}\n")
        data = "".join(rows).encode()
        export_id = "export-" + str(len(exports)+1)
        manifests = []
        for s in p["sources"]:
            m = meta(s); m["selector"] = copy.deepcopy(s); m["sourceHash"] = s["sourceHash"]; manifests.append(m)
        manifest = dict(schemaId="range.series-export/v1", methodVersion="range.series-analysis/v1", exportId=export_id,
                        scope="complete-raw", sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), count=2000*len(manifests), sources=manifests)
        exports[export_id] = (manifest, data)
        return manifest
    if op == "result.raw.export.chunk":
        manifest, data = exports[p["exportId"]]
        offset, length = p["offset"], p["length"]
        part = data[offset:offset+length]
        if bad == "chunk":
            part = b"x" * len(part)
        return dict(schemaId="range.series-export-chunk/v1", exportId=p["exportId"], sha256=manifest["sha256"],
                    bytes=manifest["bytes"], offset=offset, length=len(part), encoding="base64",
                    data=base64.b64encode(part).decode(), complete=offset+len(part)==len(data))
    if op == "fixture.query":
        return dict(calls=calls[-100:], deferred=[r["requestId"] for r, _ in deferred])
    if op == "fixture.revision":
        revision += 1
        return {}
    if op == "fixture.reverse":
        reverse = not reverse
        return {}
    if op == "fixture.bad":
        bad = p["kind"]
        return {}
    if op == "fixture.defer":
        defer = True
        return {}
    if op == "fixture.release":
        for old, result in deferred:
            respond(old, result)
        deferred.clear(); defer = False
        return {}
    raise ValueError(op)


for line in sys.stdin:
    request = json.loads(line)
    result = dispatch(request)
    if result is not None:
        respond(request, result)
