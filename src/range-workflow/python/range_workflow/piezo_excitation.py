"""PC-06 time-only nodal loads; native FE integration remains the caller's job.

The catalog must come from the actual native caller's physical DOF capabilities.
External callables are trusted in-process code, not sandboxed or proven pure.
No FE state, process launch, filesystem workspace mutation, or accepted-step state
is part of this interface. Endpoint loads must be evaluated separately by FE.
"""
from __future__ import annotations

import ast
import copy
from dataclasses import dataclass, field
from contextlib import contextmanager
from contextvars import ContextVar
import hashlib
import inspect
import json
import marshal
import math
from pathlib import Path
import struct
import sys
import threading
from types import MappingProxyType
from typing import Callable, Mapping


class ExcitationError(ValueError):
    """Invalid configuration, stale code, or failed time-only evaluation."""


_SOURCE_PATH = Path(__file__).resolve()
_SOURCE_BYTES = _SOURCE_PATH.read_bytes()
_SOURCE_HASH = hashlib.sha256(_SOURCE_BYTES).hexdigest()
_CODE_HASH = hashlib.sha256(marshal.dumps(sys._getframe().f_code)).hexdigest()


def _check_source():
    try:
        valid = _SOURCE_PATH.read_bytes() == _SOURCE_BYTES
    except OSError as exc:
        raise ExcitationError("excitation source unavailable; reload required") from exc
    if not valid:
        raise ExcitationError("excitation source changed; reload required")


def _number(value, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ExcitationError(f"{label} must be a real number")
    try:
        result = float(value)
    except (OverflowError, ValueError) as exc:
        raise ExcitationError(f"{label} is not finite binary64") from exc
    if not math.isfinite(result):
        raise ExcitationError(f"{label} must be finite")
    return result


def _keys(value, required, optional=()):
    if not isinstance(value, Mapping) or set(value) - set(required) - set(optional) or not set(required) <= set(value):
        raise ExcitationError(f"expected fields {sorted(required)}; unexpected or missing fields")


def _canonical(value):
    """Type-tagged exact binary64 encoding, preserving signed zero."""
    if isinstance(value, bool):
        return ["bool", value]
    if isinstance(value, (int, float)):
        return ["binary64", struct.pack(">d", _number(value, "identity value")).hex()]
    if isinstance(value, str):
        return ["string", value]
    if isinstance(value, Mapping):
        if not all(isinstance(k, str) for k in value):
            raise ExcitationError("identity keys must be strings")
        return ["map", [[k, _canonical(value[k])] for k in sorted(value)]]
    if isinstance(value, (list, tuple)):
        return ["list", [_canonical(v) for v in value]]
    raise ExcitationError("unsupported identity value")


def _digest(value):
    return hashlib.sha256(json.dumps(_canonical(value), separators=(",", ":"), ensure_ascii=True).encode()).hexdigest()


@dataclass(frozen=True)
class ForceChannel:
    channel_id: str
    physical_node_id: int
    dof: str
    unit: str
    coordinate_system: str = "global"

    def __post_init__(self):
        if not isinstance(self.channel_id, str) or not self.channel_id.strip():
            raise ExcitationError("channel ID must be nonempty")
        if isinstance(self.physical_node_id, bool) or not isinstance(self.physical_node_id, int) or self.physical_node_id < 0:
            raise ExcitationError("physical node ID must be a nonnegative integer")
        if self.coordinate_system not in ("global", "local"):
            raise ExcitationError("unsupported force coordinate system")
        axes = ("X", "Y", "Z", "RX", "RY", "RZ") if self.coordinate_system == "global" else ("EX", "EY", "EZ", "RX", "RY", "RZ")
        if self.dof not in axes:
            raise ExcitationError("unsupported nodal DOF")
        if self.unit != ("N" if not self.dof.startswith("R") else "N m"):
            raise ExcitationError("force/moment unit does not match DOF")


def build_force_catalog(records, *, supported_dofs):
    """Validate against {physical_node_id: iterable of actual supported DOFs}.

    Rotational moments are enabled only if the native caller explicitly advertises
    that physical rotational DOF. No equation-row or geometric ID is inferred.
    """
    if not isinstance(supported_dofs, Mapping) or not isinstance(records, (list, tuple)):
        raise ExcitationError("catalog requires records and native supported DOF mapping")
    channels = []
    ids = set()
    bindings = set()
    for record in records:
        _keys(record, ("channelId", "physicalNodeId", "dof", "unit"), ("coordinateSystem",))
        c = ForceChannel(record["channelId"], record["physicalNodeId"], record["dof"], record["unit"], record.get("coordinateSystem", "global"))
        if c.dof not in supported_dofs.get(c.physical_node_id, ()):
            raise ExcitationError("channel does not map to an actual supported native physical DOF")
        if c.channel_id in ids or (c.physical_node_id, c.coordinate_system, c.dof) in bindings:
            raise ExcitationError("duplicate channel or physical DOF catalog binding")
        ids.add(c.channel_id)
        bindings.add((c.physical_node_id, c.coordinate_system, c.dof))
        channels.append(c)
    return tuple(channels)


_FUNCTIONS = {"sin": math.sin, "cos": math.cos, "exp": math.exp, "sqrt": math.sqrt}


def _expression(source):
    if not isinstance(source, str) or len(source) > 4096:
        raise ExcitationError("expression must be a bounded string")
    try:
        tree = ast.parse(source, mode="eval")
    except (SyntaxError, RecursionError) as exc:
        raise ExcitationError("invalid time expression") from exc
    if len(list(ast.walk(tree))) > 256:
        raise ExcitationError("expression too complex")

    def compile_node(n):
        if isinstance(n, ast.Constant):
            v = _number(n.value, "expression constant")
            return lambda t: v
        if isinstance(n, ast.Name) and n.id in ("t", "pi"):
            return (lambda t: t) if n.id == "t" else (lambda t: math.pi)
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.UAdd, ast.USub)):
            f = compile_node(n.operand)
            return (lambda t: f(t)) if isinstance(n.op, ast.UAdd) else (lambda t: -f(t))
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in _FUNCTIONS and len(n.args) == 1 and not n.keywords:
            f, op = compile_node(n.args[0]), _FUNCTIONS[n.func.id]
            return lambda t: op(f(t))
        if isinstance(n, ast.BinOp):
            a, b = compile_node(n.left), compile_node(n.right)
            if isinstance(n.op, ast.Add):
                return lambda t: a(t) + b(t)
            if isinstance(n.op, ast.Sub):
                return lambda t: a(t) - b(t)
            if isinstance(n.op, ast.Mult):
                return lambda t: a(t) * b(t)
            if isinstance(n.op, ast.Div):
                return lambda t: a(t) / b(t)
            if isinstance(n.op, ast.Pow):
                # Exponents must be literal signed constants, not t-dependent.
                exponent = n.right
                if isinstance(exponent, ast.UnaryOp) and isinstance(exponent.op, (ast.UAdd, ast.USub)) and isinstance(exponent.operand, ast.Constant):
                    power = _number(exponent.operand.value, "exponent") * (-1 if isinstance(exponent.op, ast.USub) else 1)
                elif isinstance(exponent, ast.Constant):
                    power = _number(exponent.value, "exponent")
                else:
                    raise ExcitationError("power requires a literal constant exponent")
                if abs(power) > 16:
                    raise ExcitationError("power exponent exceeds bound 16")
                return lambda t: a(t) ** power
        raise ExcitationError("unsupported time expression AST")

    return compile_node(tree.body)


def _term(config):
    if not isinstance(config, Mapping) or "kind" not in config:
        raise ExcitationError("term requires kind")
    kind = config["kind"]
    if kind == "constant":
        _keys(config, ("kind", "value"))
        value = _number(config["value"], "constant")
        return lambda t: value
    if kind == "expression":
        _keys(config, ("kind", "expression"))
        return _expression(config["expression"])
    if kind == "sine":
        _keys(config, ("kind", "amplitude", "frequencyHz", "phaseRad"))
        a, f, p = (_number(config[k], k) for k in ("amplitude", "frequencyHz", "phaseRad"))
        if f < 0:
            raise ExcitationError("frequency must be nonnegative")
        return lambda t: a * math.sin(2 * math.pi * f * t + p)
    if kind == "multisine":
        _keys(config, ("kind", "components"))
        if not isinstance(config["components"], (list, tuple)) or not config["components"]:
            raise ExcitationError("multisine requires components")
        fs = []
        for c in config["components"]:
            _keys(c, ("amplitude", "frequencyHz", "phaseRad"))
            fs.append(_term(dict(c, kind="sine")))
        return lambda t: math.fsum(f(t) for f in fs)
    if kind == "chirp":
        _keys(config, ("kind", "amplitude", "startTime", "duration", "startFrequencyHz", "endFrequencyHz", "phaseRad"))
        a, start, duration, f0, f1, p = (_number(config[k], k) for k in ("amplitude", "startTime", "duration", "startFrequencyHz", "endFrequencyHz", "phaseRad"))
        if duration <= 0 or min(f0, f1) < 0:
            raise ExcitationError("chirp requires positive duration and nonnegative frequencies")
        # Windowed linear frequency sweep, zero outside [start,start+duration].
        def chirp(t):
            tau = t - start
            return 0.0 if tau < 0 or tau > duration else a * math.sin(p + 2 * math.pi * (f0 * tau + 0.5 * (f1 - f0) * tau * tau / duration))
        return chirp
    if kind == "smoothPulse":
        _keys(config, ("kind", "amplitude", "startTime", "riseTime", "holdTime", "fallTime"))
        a, start, rise, hold, fall = (_number(config[k], k) for k in ("amplitude", "startTime", "riseTime", "holdTime", "fallTime"))
        if rise <= 0 or fall <= 0 or hold < 0:
            raise ExcitationError("pulse requires positive ramps and nonnegative hold")
        def smooth(x):
            return x * x * x * (10 + x * (-15 + 6 * x))
        def pulse(t):
            tau = t - start
            if tau <= 0 or tau >= rise + hold + fall:
                return 0.0
            if tau < rise:
                return a * smooth(tau / rise)
            if tau <= rise + hold:
                return a
            return a * smooth((rise + hold + fall - tau) / fall)
        return pulse
    if kind == "table":
        _keys(config, ("kind", "times", "values", "interpolation", "outOfRange"))
        if config["interpolation"] != "linear" or config["outOfRange"] not in ("error", "hold", "zero"):
            raise ExcitationError("table requires linear interpolation and explicit error/hold/zero policy")
        if not isinstance(config["times"], (list, tuple)) or not isinstance(config["values"], (list, tuple)):
            raise ExcitationError("table requires arrays")
        xs = tuple(_number(x, "table time") for x in config["times"])
        ys = tuple(_number(y, "table force") for y in config["values"])
        if len(xs) < 2 or len(xs) != len(ys) or any(b <= a for a, b in zip(xs, xs[1:])):
            raise ExcitationError("table needs matching arrays and strictly increasing times")
        policy = config["outOfRange"]
        def table(t):
            if t < xs[0] or t > xs[-1]:
                if policy == "error":
                    raise ExcitationError("time outside force table")
                return 0.0 if policy == "zero" else ys[0 if t < xs[0] else -1]
            import bisect
            i = min(bisect.bisect_right(xs, t) - 1, len(xs) - 2)
            ratio = (t - xs[i]) / (xs[i + 1] - xs[i])
            return (1 - ratio) * ys[i] + ratio * ys[i + 1]
        return table
    raise ExcitationError("unknown excitation term kind")


@dataclass(frozen=True)
class ExternalTimeFunction:
    """Frozen identity for a trusted function of t; globals purity is not proven.

    Exact-time memoization is optional derivative caching only. Revision checks
    run even on cache hits. Caller owns declared dependency/environment validity.
    """
    function: Callable = field(repr=False, compare=False)
    code_identity: str
    dependency_identity: str
    environment_identity: str
    channels: tuple[ForceChannel, ...]
    missing_policy: str
    cache_exact_time: bool = False
    revision_check: Callable | None = field(default=None, repr=False, compare=False)
    source_bytes: bytes = field(init=False, repr=False)
    source_hash: str = field(init=False)
    loaded_code_hash: str = field(init=False)
    identity_hash: str = field(init=False)
    _source_path: Path | None = field(init=False, repr=False)
    _cache: dict = field(default_factory=dict, init=False, repr=False, compare=False)
    _observed: dict = field(default_factory=dict, init=False, repr=False, compare=False)

    def __post_init__(self):
        if not all(isinstance(v, str) and v.strip() for v in (self.code_identity, self.dependency_identity, self.environment_identity)):
            raise ExcitationError("external code/dependency/environment identities are required")
        if type(self.cache_exact_time) is not bool or self.missing_policy not in ("error", "zero"):
            raise ExcitationError("explicit external missing policy and boolean cache option required")
        channels = tuple(self.channels)
        if not channels or any(not isinstance(c, ForceChannel) for c in channels) or len({c.channel_id for c in channels}) != len(channels):
            raise ExcitationError("external declared channels must be unique and nonempty")
        object.__setattr__(self, "channels", channels)
        if not inspect.isfunction(self.function) or inspect.iscoroutinefunction(self.function) or inspect.isgeneratorfunction(self.function):
            raise ExcitationError("external callable must be a Python function")
        parameters = tuple(inspect.signature(self.function).parameters.values())
        if len(parameters) != 1 or parameters[0].name != "t" or parameters[0].kind not in (inspect.Parameter.POSITIONAL_ONLY, inspect.Parameter.POSITIONAL_OR_KEYWORD) or parameters[0].default is not inspect.Parameter.empty:
            raise ExcitationError("external signature must take only required t")
        path = inspect.getsourcefile(self.function)
        path = Path(path).resolve() if path and Path(path).is_file() else None
        data = path.read_bytes() if path else b""
        # v2 omits marshal reference flags that depend on transient refcounts
        # (for example strings retained by the just-returned force dictionary).
        code_hash = hashlib.sha256(marshal.dumps(self.function.__code__, 2)).hexdigest()
        object.__setattr__(self, "_source_path", path)
        object.__setattr__(self, "source_bytes", data)
        object.__setattr__(self, "source_hash", hashlib.sha256(data).hexdigest())
        object.__setattr__(self, "loaded_code_hash", code_hash)
        object.__setattr__(self, "identity_hash", _digest({"code": self.code_identity, "dependency": self.dependency_identity, "environment": self.environment_identity, "source": self.source_hash, "loadedCode": code_hash, "channels": [[c.channel_id, str(c.physical_node_id), c.dof, c.unit, c.coordinate_system] for c in channels], "missing": self.missing_policy, "cache": self.cache_exact_time}))

    def _check_revision(self):
        if self.revision_check is not None:
            self.revision_check()
        try:
            stale = self._source_path is not None and self._source_path.read_bytes() != self.source_bytes
        except OSError as exc:
            raise ExcitationError("external source unavailable") from exc
        parameters = tuple(inspect.signature(self.function).parameters.values())
        signature_stale = len(parameters) != 1 or parameters[0].name != "t" or parameters[0].kind not in (inspect.Parameter.POSITIONAL_ONLY, inspect.Parameter.POSITIONAL_OR_KEYWORD) or parameters[0].default is not inspect.Parameter.empty
        if stale or signature_stale or hashlib.sha256(marshal.dumps(self.function.__code__, 2)).hexdigest() != self.loaded_code_hash:
            raise ExcitationError("external function revision changed; recompile required")

    def evaluate(self, t):
        t = _number(t, "time")
        self._check_revision()
        key = struct.pack(">d", t)
        try:
            from .piezo_external_time import guarded_output
            with guarded_output():
                raw = self.function(t)
                if not isinstance(raw, Mapping) or set(raw) - {c.channel_id for c in self.channels}:
                    raise ExcitationError("external output has unknown channels")
                values = {}
                for c in self.channels:
                    if c.channel_id not in raw:
                        if self.missing_policy != "zero":
                            raise ExcitationError("external output missing declared channel")
                        values[c.channel_id] = 0.0
                    else:
                        _keys(raw[c.channel_id], ("value", "unit"))
                        if raw[c.channel_id]["unit"] != c.unit:
                            raise ExcitationError("external force unit mismatch")
                        values[c.channel_id] = _number(raw[c.channel_id]["value"], "external force")
        except ExcitationError:
            raise
        except BaseException as exc:
            raise ExcitationError("external time function failed") from exc
        self._check_revision()
        result = MappingProxyType(values)
        signature = tuple(struct.pack(">d", values[c.channel_id]) for c in self.channels)
        if key in self._observed and self._observed[key] != signature:
            raise ExcitationError("external time function is observably nondeterministic at repeated binary64 time")
        # Bounded observation window; this detects observed retries, not purity.
        self._observed[key] = signature
        if len(self._observed) > 4096:
            oldest = next(iter(self._observed))
            del self._observed[oldest]
            self._cache.pop(oldest, None)
        if self.cache_exact_time:
            # Replay the verified immutable first result; reevaluation above is
            # still required so caching cannot hide observable nondeterminism.
            return self._cache.setdefault(key, result)
        return result


# Provider-private bounded framework profile. External function/dependency/code
# revisions remain checked at every point; this never represents accepted state.
_FRAMEWORK_BATCH = ContextVar("range_piezo_framework_batch", default=None)


class _FrameworkBatch:
    __slots__ = ("proposal", "remaining", "owner", "active")

    def __init__(self, proposal, maximum):
        self.proposal, self.remaining = proposal, maximum
        self.owner, self.active = threading.get_ident(), True

    def matches(self, proposal):
        return self.active and self.proposal is proposal and self.owner == threading.get_ident()


@dataclass(frozen=True)
class ExcitationProposal:
    catalog: tuple[ForceChannel, ...]
    configuration_bytes: bytes
    source_hash: str
    loaded_code_hash: str
    _terms: tuple = field(repr=False, compare=False)
    _external: tuple = field(repr=False, compare=False)
    _sampled: tuple = field(default=(), repr=False, compare=False)

    @contextmanager
    def _framework_batch(self, maximum=129):
        """Delay framework-file revalidation only within one bounded call.

        Loaded framework code and source identity remain frozen. This is a
        before/after compatibility check, not authentication or transient-write
        monitoring. Standalone points retain their original file check.
        """
        if type(maximum) is not int or maximum < 0 or maximum > 129:
            raise ExcitationError("framework batch must allow between 0 and 129 points")
        if _FRAMEWORK_BATCH.get() is not None:
            raise ExcitationError("framework batch cannot nest")
        _check_source()
        scope = _FrameworkBatch(self, maximum)
        token = _FRAMEWORK_BATCH.set(scope)
        try:
            yield
        finally:
            scope.active = False
            _FRAMEWORK_BATCH.reset(token)
            _check_source()

    def check_revision(self):
        scope = _FRAMEWORK_BATCH.get()
        if scope is None or not scope.matches(self):
            _check_source()
        for external in self._external:
            external._check_revision()
        if self._sampled:
            from .piezo_external_io import fingerprint
            for source in self._sampled:
                if fingerprint(source["connection"]) != source["connectionFingerprint"]:
                    raise ExcitationError("external sampled source identity changed; reload required")

    def evaluate_frame(self, frame):
        _keys(frame, ("time",))
        return self.evaluate(frame["time"])

    def evaluate(self, t, *, sampled_values=None):
        if self._sampled and sampled_values is None:
            raise ExcitationError("sampled excitation requires a frozen FE interval ledger")
        scope = _FRAMEWORK_BATCH.get()
        if scope is not None and scope.matches(self):
            if scope.remaining == 0:
                raise ExcitationError("framework batch point bound exceeded")
            scope.remaining -= 1
        self.check_revision()
        t = _number(t, "time")
        values = {c.channel_id: [] for c in self.catalog}
        try:
            for channel, functions in self._terms:
                values[channel].extend(_number(f(t), "force term") for f in functions)
            for external in self._external:
                for channel, value in external.evaluate(t).items():
                    values[channel].append(value)
            if sampled_values is not None:
                expected = {cid for source in self._sampled for cid in source["channelIds"]}
                if set(sampled_values) != expected:
                    raise ExcitationError("sampled excitation ports differ from frozen mapping")
                for channel, value in sampled_values.items():
                    values[channel].append(_number(value, "sampled force"))
            forces = {c.channel_id: {"value": _number(math.fsum(values[c.channel_id]), "total force"), "unit": c.unit} for c in self.catalog}
        except ExcitationError:
            raise
        except (ArithmeticError, ValueError, TypeError) as exc:
            raise ExcitationError("time excitation evaluation failed or overflowed") from exc
        return {"time": t, "forces": forces, "sourceHash": self.source_hash, "loadedCodeHash": self.loaded_code_hash}


def compile_excitation(catalog, configuration=None, *, external_functions=()):
    """Compile atomically, with no accepted-step/workspace changes.

    Configuration includes channels with explicit additive terms and optional
    externalFunctions using piezo_external_time's frozen JSON source contract.
    Unconfigured catalog channels are zero (free vibration default).
    """
    _check_source()
    catalog = tuple(catalog)
    if any(not isinstance(c, ForceChannel) for c in catalog) or len({c.channel_id for c in catalog}) != len(catalog) or len({(c.physical_node_id, c.coordinate_system, c.dof) for c in catalog}) != len(catalog):
        raise ExcitationError("invalid or duplicate force catalog")
    by_id = {c.channel_id: c for c in catalog}
    configuration = {"channels": []} if configuration is None else configuration
    _keys(configuration, ("channels",), ("externalFunctions", "externalSources"))
    if not isinstance(configuration["channels"], (list, tuple)):
        raise ExcitationError("channels configuration must be an array")
    terms = []
    seen = set()
    for channel in configuration["channels"]:
        _keys(channel, ("channelId", "unit", "terms"))
        cid = channel["channelId"]
        if not isinstance(cid, str) or cid not in by_id or cid in seen:
            raise ExcitationError("unknown or duplicate configured channel")
        if channel["unit"] != by_id[cid].unit or not isinstance(channel["terms"], (list, tuple)) or not channel["terms"]:
            raise ExcitationError("channel requires matching unit and explicit nonempty additive terms")
        seen.add(cid)
        terms.append((cid, tuple(_term(term) for term in channel["terms"])))
    from .piezo_external_time import load_external_functions
    external_functions = tuple(external_functions) + load_external_functions(
        configuration.get("externalFunctions", []), catalog)
    external_seen = set()
    for external in external_functions:
        if not isinstance(external, ExternalTimeFunction):
            raise ExcitationError("external functions must be frozen wrappers")
        external._check_revision()
        if any(by_id.get(c.channel_id) != c for c in external.channels):
            raise ExcitationError("external declared channels mismatch actual catalog")
        ids = {c.channel_id for c in external.channels}
        if ids & external_seen:
            raise ExcitationError("duplicate external channel declaration")
        external_seen.update(ids)
    sampled = []
    sources = configuration.get("externalSources", [])
    if not isinstance(sources, list):
        raise ExcitationError("externalSources must be an array")
    if sources:
        from .piezo_external_io import normalize_config, fingerprint
        for source in sources:
            _keys(source, ("connection", "channelIds"), ("sampleEveryFESteps", "seed", "initialState"))
            ids = source["channelIds"]
            if (not isinstance(ids, list) or not ids or any(not isinstance(cid, str) or cid not in by_id for cid in ids)
                    or len(set(ids)) != len(ids) or set(ids) & external_seen):
                raise ExcitationError("unknown or duplicate sampled excitation channel ownership")
            every = source.get("sampleEveryFESteps", 1)
            seed = source.get("seed", 0)
            if type(every) is not int or every < 1 or type(seed) is not int or seed < 0:
                raise ExcitationError("sampleEveryFESteps and seed require bounded integer values")
            initial = copy.deepcopy(source.get("initialState", {}))
            _canonical(initial)
            connection = normalize_config(source["connection"])
            sampled.append(dict(connection=connection, connectionFingerprint=fingerprint(connection),
                channelIds=ids[:], sampleEveryFESteps=every, seed=seed, initialState=initial))
            external_seen.update(ids)
    identity = {"catalog": [[c.channel_id, str(c.physical_node_id), c.dof, c.unit, c.coordinate_system] for c in catalog], "configuration": configuration, "external": [e.identity_hash for e in external_functions], "sampled": sampled, "source": _SOURCE_HASH}
    canonical_bytes = json.dumps(_canonical(identity), separators=(",", ":")).encode()
    loaded_code = (_digest({"module": _CODE_HASH, "external": [e.loaded_code_hash for e in external_functions]})
                   if external_functions else _CODE_HASH)
    return ExcitationProposal(catalog, canonical_bytes, hashlib.sha256(canonical_bytes).hexdigest(), loaded_code, tuple(terms), external_functions, tuple(sampled))
