"""Frozen WF-11 numeric boundary; no lifecycle, RPC or scientific imports.

FFT is unnormalised exp(-2*pi*i*k*j/N). Periodic Hann, one-sided amplitude
2*abs(FFT)/sum(window), PSD 2*abs(FFT)^2/(fs*sum(window^2)); DC and
even-N Nyquist are never doubled. Filter precedes mean removal/windowing.
"""
from __future__ import annotations

import hashlib
import json
import math
import re

PROTOCOL_VERSION = "range.extension/v2"
INPUT_SCHEMA = PROTOCOL_VERSION + "/multichannel-series"
OUTPUT_SCHEMA = PROTOCOL_VERSION + "/spectral-result"
ALGORITHM = "spectral-filter/v1"
MAX_POINTS = 10_000
MAX_CHANNELS = 4
MAX_JSON_BYTES = 32 * 1024 * 1024
DEPENDENCIES = {"numpy": "2.2.6", "scipy": "1.15.3"}
UNITS = {"displacement": "m", "velocity": "m/s", "acceleration": "m/s^2",
         "force": "N", "energy": "J", "controlWork": "J", "dimensionless": "1"}
_ID = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
_HASH = re.compile(r"^[0-9a-f]{64}$")


class ScientificContractError(ValueError):
    pass


def exact(value, keys):
    if type(value) is not dict or set(value) != set(keys):
        raise ScientificContractError("missing or unknown fields")
    return value


def number(value, *, input_limit=True):
    if type(value) not in (int, float):
        raise ScientificContractError("expected finite real number")
    try:
        result = float(value)
    except (OverflowError, ValueError):
        raise ScientificContractError("expected finite real number") from None
    if not math.isfinite(result) or (input_limit and abs(result) > 1e100):
        raise ScientificContractError("number outside supported finite range")
    return result


def near(a, b, *, atol=1e-12, rtol=1e-10):
    return abs(a-b) <= atol + rtol * abs(b)


def _array(value, count, *, input_limit=True):
    if type(value) is not list or len(value) != count:
        raise ScientificContractError("invalid array dimensions")
    return [number(x, input_limit=input_limit) for x in value]


def _id(value):
    if type(value) is not str or not _ID.fullmatch(value):
        raise ScientificContractError("invalid identifier")
    return value


def strict_json(raw):
    if type(raw) is not bytes or len(raw) > MAX_JSON_BYTES:
        raise ScientificContractError("JSON exceeds 32 MiB")
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ScientificContractError("duplicate JSON key")
            result[key] = value
        return result
    try:
        return json.loads(raw.decode("utf-8", "strict"), object_pairs_hook=pairs, parse_float=lambda text: number(float(text), input_limit=False),
                          parse_constant=lambda x: (_ for _ in ()).throw(ScientificContractError("nonfinite JSON")))
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ScientificContractError("invalid strict JSON") from exc


def canonical_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=False, allow_nan=False).encode("utf-8")).hexdigest()


def verification_profile():
    return {"id": "range.scientific-verification/v1", "algorithm": ALGORITHM,
            "smallNOracle": "stdlib-dft-and-recurrence", "smallNLimit": 128,
            "largeNReference": "host-numpy-rfft-and-recurrence",
            "sharedLibrary": "NumPy FFT library; not a fully independent large-N FFT",
            "analyticProperties": ["DC", "sine-phase", "Nyquist", "odd-even-end-bin", "Hann-Parseval", "filter-impulse-step"],
            "absoluteTolerance": 1e-9, "relativeTolerance": 1e-8,
            "roundoffBound": "16*binary64-epsilon*N*(1+ceil(log2(N)))*max(abs(filtered)); propagated to amplitude and PSD",
            "smallNDC": "center rectangular history; analytic periodic-Hann DC contribution"}


CONTRACT_SPEC = {"protocol": PROTOCOL_VERSION, "input": INPUT_SCHEMA, "output": OUTPUT_SCHEMA,
                 "algorithm": ALGORITHM, "maximumPoints": MAX_POINTS, "maximumChannels": MAX_CHANNELS,
                 "dependencies": DEPENDENCIES, "units": UNITS,
                 "inputFields": ["schema", "algorithm", "domain", "scalarType", "times", "timeUnit", "samplePeriodS", "samplingRateHz", "channels", "window", "removeMean", "filter"],
                 "channelFields": ["id", "quantity", "axis", "unit", "values", "optional:sourceHash/artifactId/channelId"],
                 "outputArrays": ["times", "frequenciesHz", "channels.filteredValues", "channels.fft:[real,imag]", "channels.amplitude", "channels.psd"],
                 "psdUnit": {"numeratorUnit": "input unit", "power": 2, "denominatorUnit": "Hz"},
                 "window": "rectangular or periodic Hann", "fft": "unnormalized negative exponential rfft",
                 "scales": "one-sided amplitude/coherent gain and PSD/window energy; endpoints undoubled",
                 "filter": "bilinear first-order Butterworth; causal zero state; filter then remove mean then window",
                 "verificationProfile": verification_profile()}
CONTRACT_HASH = canonical_hash(CONTRACT_SPEC)


def contract_hash():
    return CONTRACT_HASH


def validate_input(value):
    exact(value, {"schema", "algorithm", "domain", "scalarType", "times", "timeUnit",
                  "samplePeriodS", "samplingRateHz", "channels", "window", "removeMean", "filter"})
    if (value["schema"] != INPUT_SCHEMA or value["algorithm"] != ALGORITHM or
            value["domain"] != "time" or value["scalarType"] != "real64" or value["timeUnit"] != "s"):
        raise ScientificContractError("unsupported scientific schema, algorithm, domain or type")
    if type(value["times"]) is not list or not 2 <= len(value["times"]) <= MAX_POINTS:
        raise ScientificContractError("time grid requires 2..10000 samples")
    n = len(value["times"])
    times = _array(value["times"], n)
    dt, fs = number(value["samplePeriodS"]), number(value["samplingRateHz"])
    if dt <= 0 or fs <= 0 or not near(dt*fs, 1) or not math.isfinite(fs*fs):
        raise ScientificContractError("invalid sampling rate/period")
    if any(b <= a or not near(b-a, dt, atol=abs(dt)*1e-10, rtol=1e-8) for a,b in zip(times,times[1:])):
        raise ScientificContractError("time grid must be uniform and increasing")
    channels = value["channels"]
    if type(channels) is not list or not 1 <= len(channels) <= MAX_CHANNELS:
        raise ScientificContractError("requires 1..4 channels")
    copied, ids = [], set()
    for channel in channels:
        if type(channel) is not dict:
            raise ScientificContractError("invalid channel")
        required = {"id", "quantity", "axis", "unit", "values"}
        exact(channel, required | ({"source"} if "source" in channel else set()))
        ident = _id(channel["id"])
        if ident in ids:
            raise ScientificContractError("duplicate channel id")
        ids.add(ident)
        quantity = channel["quantity"]
        if type(quantity) is not str or quantity not in UNITS or channel["unit"] != UNITS[quantity]:
            raise ScientificContractError("quantity/unit mismatch")
        if channel["axis"] not in ("x", "y", "z", "scalar") or (quantity in ("energy", "controlWork", "dimensionless") and channel["axis"] != "scalar"):
            raise ScientificContractError("invalid physical axis")
        item = dict(channel, values=_array(channel["values"], n))
        if "source" in channel:
            source = exact(channel["source"], {"sourceHash", "artifactId", "channelId"})
            if type(source["sourceHash"]) is not str or not _HASH.fullmatch(source["sourceHash"]):
                raise ScientificContractError("invalid source digest")
            item["source"] = {"sourceHash": source["sourceHash"], "artifactId": _id(source["artifactId"]), "channelId": _id(source["channelId"])}
        copied.append(item)
    if value["window"] not in ("rectangular", "hann") or type(value["removeMean"]) is not bool:
        raise ScientificContractError("invalid preprocessing options")
    filt = value["filter"]
    if type(filt) is not dict:
        raise ScientificContractError("invalid filter")
    if filt.get("kind") == "none":
        exact(filt, {"kind"})
    else:
        exact(filt, {"kind", "order", "cutoffHz", "phase", "initialCondition"})
        if (filt["kind"] != "butterworth-lowpass" or type(filt["order"]) is not int or filt["order"] != 1 or
                filt["phase"] != "causal" or filt["initialCondition"] != "zero" or not 0 < number(filt["cutoffHz"]) < fs/2):
            raise ScientificContractError("unsupported filter/boundary conditions")
    return dict(value, times=times, samplePeriodS=dt, samplingRateHz=fs, channels=copied, filter=dict(filt))


def validate_output(value, data):
    data = validate_input(data)
    exact(value, {"schema", "algorithm", "contractHash", "inputHash", "domain", "scalarType", "times", "timeUnit",
                  "samplePeriodS", "samplingRateHz", "frequenciesHz", "frequencyUnit", "binSpacingHz", "binCount", "sampleCount",
                  "window", "removeMean", "filter", "windowSum", "windowEnergy", "channels"})
    n, fs = len(data["times"]), data["samplingRateHz"]
    bins = n//2+1
    # Strictly revalidate echoed filter, including bool-versus-integer order.
    validate_input(dict(data, filter=value["filter"]))
    for key, expected in {"schema": OUTPUT_SCHEMA, "algorithm": ALGORITHM, "contractHash": CONTRACT_HASH,
                          "inputHash": canonical_hash(data), "domain": "frequency", "scalarType": "complex128-pairs",
                          "timeUnit": "s", "frequencyUnit": "Hz", "window": data["window"],
                          "removeMean": data["removeMean"], "filter": data["filter"]}.items():
        if value[key] != expected or type(value[key]) is not type(expected):
            raise ScientificContractError("output semantics differ from input")
    if type(value["sampleCount"]) is not int or value["sampleCount"] != n:
        raise ScientificContractError("invalid sample count")
    if type(value["binCount"]) is not int or value["binCount"] != bins:
        raise ScientificContractError("invalid bin count")
    for key, expected in {"samplePeriodS": data["samplePeriodS"], "samplingRateHz": fs, "binSpacingHz": fs/n,
                          "windowSum": float(n) if data["window"] == "rectangular" else n/2,
                          "windowEnergy": float(n) if data["window"] == "rectangular" else (1.0 if n == 2 else 3*n/8)}.items():
        tolerance = abs(expected)*1e-12 if key in ("samplePeriodS", "samplingRateHz", "binSpacingHz") else 1e-12
        if not near(number(value[key]), expected, atol=tolerance):
            raise ScientificContractError("invalid sampling/window normalization")
    if _array(value["times"], n) != data["times"]:
        raise ScientificContractError("filtered history grid mismatch")
    if any(not near(f, k*fs/n, atol=abs(fs)*1e-12) for k,f in enumerate(_array(value["frequenciesHz"], bins))):
        raise ScientificContractError("invalid frequency grid or endpoint")
    if type(value["channels"]) is not list or len(value["channels"]) != len(data["channels"]):
        raise ScientificContractError("output channel dimensions mismatch")
    for actual, source in zip(value["channels"], data["channels"]):
        exact(actual, {"id", "quantity", "axis", "unit", "amplitudeUnit", "psdUnit", "filteredValues", "fft", "amplitude", "psd"})
        for key in ("id", "quantity", "axis", "unit"):
            if actual[key] != source[key]:
                raise ScientificContractError("channel semantics mismatch")
        psd_unit = exact(actual["psdUnit"], {"numeratorUnit", "power", "denominatorUnit"})
        if type(psd_unit["power"]) is not int or actual["amplitudeUnit"] != source["unit"] or psd_unit != {"numeratorUnit": source["unit"], "power": 2, "denominatorUnit": "Hz"}:
            raise ScientificContractError("spectral unit mismatch")
        _array(actual["filteredValues"], n, input_limit=False)
        amp, psd = _array(actual["amplitude"], bins, input_limit=False), _array(actual["psd"], bins, input_limit=False)
        if type(actual["fft"]) is not list or len(actual["fft"]) != bins:
            raise ScientificContractError("invalid complex FFT dimensions")
        for k, pair in enumerate(actual["fft"]):
            real, imag = _array(pair, 2, input_limit=False)
            endpoint = k == 0 or (n % 2 == 0 and k == n//2)
            if endpoint and not near(imag, 0, atol=1e-9):
                raise ScientificContractError("real FFT endpoint must be real")
            factor = 1 if endpoint else 2
            magnitude = math.hypot(real, imag)
            expected_amp = number(factor*magnitude/number(value["windowSum"]), input_limit=False)
            expected_psd = number(factor*magnitude*magnitude/(fs*number(value["windowEnergy"])), input_limit=False)
            if amp[k] < 0 or psd[k] < 0 or not near(amp[k], expected_amp, atol=1e-9) or not near(psd[k], expected_psd, atol=1e-9):
                raise ScientificContractError("invalid amplitude/PSD normalization")
    return value
