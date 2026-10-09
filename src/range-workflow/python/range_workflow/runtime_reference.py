"""Independent matrix Newmark reference driven by accepted parameter history."""
from __future__ import annotations

import math
import numpy as np

from . import runtime_parameters as rp


def accepted_schedule(state, proposal_records):
    """Resolve every accepted digest to its durable, pre-effect host proposal.

    An unobserved submission may become an accepted fact only because an actual
    checkpoint/native state contains its exact ID and digest. Missing history
    values are an error rather than a constant-gain approximation.
    """
    state = rp.validate_state(state)
    result = []
    for change in state["history"]:
        record = proposal_records.get(change["proposalId"])
        if type(record) is not dict or not record.get("submittedAt"):
            raise ValueError("Accepted runtime history has no durable submitted parameter values")
        p = rp.validate_parameters(record["parameters"])
        if (p["parameterSha256"] != change["parameterSha256"] or p["revision"] != change["revision"]
                or p["fixedBaseControllerRevision"] != state["parameters"]["fixedBaseControllerRevision"]):
            raise ValueError("Accepted runtime history differs from durable proposed values")
        result.append({**change, "parameters": p})
    return result


def replay(mass, damping, stiffness, force_column, velocity_row, q0, v0,
           *, dt, steps, initial_parameters, schedule):
    """Average acceleration, force sampled and held over each accepted interval.

    Acceleration is refreshed with that interval's force, matching the frozen
    Newmark physical contract. No solver frames or trajectories enter this law.
    """
    if type(steps) is not int or not 1 <= steps <= 5000 or type(dt) not in (int, float) or not math.isfinite(dt) or dt <= 0:
        raise ValueError("Invalid reference clock")
    p = rp.validate_parameters(initial_parameters)
    if len(p["actuatorLimits"]) != 1:
        raise ValueError("Runtime reference supports one scalar force actuator")
    arrays = [np.asarray(value, dtype=np.float64) for value in
              (mass, damping, stiffness, force_column, velocity_row, q0, v0)]
    m, c, k, b, sensor, q, v = arrays
    n = q.size
    if (not 1 <= n <= 256 or q.shape != (n,) or v.shape != (n,) or b.shape != (n,)
            or sensor.shape != (n,) or any(a.shape != (n, n) for a in (m, c, k))
            or any(not np.isfinite(a).all() for a in arrays)):
        raise ValueError("Invalid independent reference matrices or coordinates")
    if not np.allclose(m, m.T, rtol=1e-12, atol=1e-14) or not np.allclose(k, k.T, rtol=1e-12, atol=1e-14):
        raise ValueError("Reference mass and stiffness must be symmetric")
    np.linalg.cholesky(m)
    changes = {}
    previous_seq, previous_time = 0, -math.inf
    for change in schedule:
        seq = rp.decimal(change["appliedSeq"])
        time = float(change["acceptedTimeS"])
        next_p = rp.validate_parameters(change["parameters"])
        if (not previous_seq < seq <= steps or not math.isfinite(time) or time <= previous_time
                or not math.isclose(time, seq * dt, rel_tol=1e-10, abs_tol=1e-10)
                or next_p["fixedBaseControllerRevision"] != p["fixedBaseControllerRevision"]
                or next_p["revision"] != str(len(changes) + 1)
                or next_p["parameterSha256"] != change["parameterSha256"]):
            raise ValueError("Runtime reference schedule is inconsistent with accepted clock")
        changes[seq] = next_p
        previous_seq, previous_time = seq, time
    effective = k + (2 / dt) * c + (4 / (dt * dt)) * m
    q, v = q.copy(), v.copy()
    last_force, work = 0.0, 0.0
    q_history, v_history, a_history, force_history = [q.tolist()], [v.tolist()], [], []
    revision_history = []
    for seq in range(1, steps + 1):
        p = changes.get(seq, p)
        limit = p["actuatorLimits"][0]
        lower, upper = limit["minN"], limit["maxN"]
        if limit["rateNPerS"] > 0:
            excursion = limit["rateNPerS"] * dt
            lower, upper = max(lower, last_force - excursion), min(upper, last_force + excursion)
        if lower > upper:
            raise ValueError("Accepted runtime limits have an empty feasible force interval")
        requested = -p["velocityGainNsm"] * float(sensor @ v)
        if not math.isfinite(requested):
            raise ValueError("Runtime reference force is nonfinite")
        force = min(upper, max(lower, requested))
        applied = b * force
        acceleration = np.linalg.solve(m, applied - c @ v - k @ q)
        if not a_history:
            a_history.append(acceleration.tolist())
        q_predict = q + dt * v + .25 * dt * dt * acceleration
        v_predict = v + .5 * dt * acceleration
        rhs = applied + m @ ((4 / (dt * dt)) * q_predict) + c @ ((2 / dt) * q_predict - v_predict)
        q_next = np.linalg.solve(effective, rhs)
        a_next = (4 / (dt * dt)) * (q_next - q_predict)
        v_next = v_predict + .5 * dt * a_next
        work += force * float(b @ (q_next - q))
        if not math.isfinite(work) or any(not np.isfinite(a).all() for a in (q_next, v_next, a_next)):
            raise ValueError("Runtime reference state is nonfinite")
        q, v, last_force = q_next, v_next, force
        q_history.append(q.tolist())
        v_history.append(v.tolist())
        a_history.append(a_next.tolist())
        force_history.append(force)
        revision_history.append(p["revision"])
    return {"q": q.tolist(), "v": v.tolist(), "a": a_history[-1],
            "controllerWork": work, "energy": float(.5 * (v @ m @ v + q @ k @ q)),
            "trajectory": {"q": q_history, "v": v_history, "a": a_history,
                           "forceN": force_history, "parameterRevision": revision_history}}
