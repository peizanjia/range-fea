"""Host scientific oracle: stdlib DFT at small N, NumPy FFT at large N.

The large-N oracle deliberately shares the pinned NumPy library with the
worker environment. Its filter recurrence and normalization are host-owned;
the small-N DFT plus analytic tests supply independent FFT evidence.
"""
from __future__ import annotations

import math
from .scientific_contract import (ALGORITHM, CONTRACT_HASH, OUTPUT_SCHEMA,
                                  ScientificContractError, canonical_hash,
                                  validate_input, validate_output, verification_profile)


def filter_history(values, sampling_rate, specification):
    if specification["kind"] == "none":
        return list(values)
    k = math.tan(math.pi * specification["cutoffHz"] / sampling_rate)
    b = k/(1+k)
    pole = (1-k)/(1+k)
    previous_input = previous_output = 0.0
    result = []
    for current in values:
        output = b*(current + previous_input) + pole*previous_output
        result.append(output)
        previous_input, previous_output = current, output
    return result


def _dft(values):
    n = len(values)
    # The transform of a constant is analytic. Removing it avoids multiplying
    # large DC values by approximate roots of unity at otherwise empty bins.
    mean = math.fsum(values)/n
    centered = [x-mean for x in values]
    return [complex(math.fsum(values), 0) if k == 0 else
            complex(math.fsum(x*math.cos(2*math.pi*k*j/n) for j,x in enumerate(centered)),
                    -math.fsum(x*math.sin(2*math.pi*k*j/n) for j,x in enumerate(centered)))
            for k in range(n//2+1)]


def reference_result(value, *, small_n=False):
    data = validate_input(value)
    n, fs = len(data["times"]), data["samplingRateHz"]
    if small_n and n > 128:
        raise ScientificContractError("stdlib DFT oracle is bounded to 128 samples")
    window = ([1.0]*n if data["window"] == "rectangular" else
              [0.5-0.5*math.cos(2*math.pi*j/n) for j in range(n)])
    total, energy = math.fsum(window), math.fsum(x*x for x in window)
    channels = []
    for channel in data["channels"]:
        filtered = filter_history(channel["values"], fs, data["filter"])
        mean = math.fsum(filtered)/n if data["removeMean"] else 0.0
        prepared = [(x-mean)*w for x,w in zip(filtered,window)]
        if small_n:
            if data["window"] == "hann":
                # Periodic Hann times a constant has only DC and bin 1.
                offset = math.fsum(filtered)/n - mean
                transformed = _dft([(x-mean-offset)*w for x,w in zip(filtered,window)])
                transformed[0] += offset*n/2
                transformed[1] -= offset*n/(2 if n == 2 else 4)
            else:
                transformed = _dft(prepared)
        else:
            import numpy as np
            transformed = np.fft.rfft(np.asarray(prepared, dtype=np.float64))
        fft, amplitude, psd = [], [], []
        for k,z in enumerate(transformed):
            endpoint = k == 0 or (n%2 == 0 and k == n//2)
            real, imag = float(z.real), 0.0 if endpoint else float(z.imag)
            magnitude = math.hypot(real, imag)
            factor = 1 if endpoint else 2
            fft.append([real, imag])
            amplitude.append(factor*magnitude/total)
            psd.append(factor*magnitude*magnitude/(fs*energy))
        channels.append({**{key: channel[key] for key in ("id", "quantity", "axis", "unit")},
                         "amplitudeUnit": channel["unit"],
                         "psdUnit": {"numeratorUnit": channel["unit"], "power": 2, "denominatorUnit": "Hz"},
                         "filteredValues": filtered, "fft": fft, "amplitude": amplitude, "psd": psd})
    result = {"schema": OUTPUT_SCHEMA, "algorithm": ALGORITHM, "contractHash": CONTRACT_HASH,
              "inputHash": canonical_hash(data), "domain": "frequency", "scalarType": "complex128-pairs",
              "times": data["times"], "timeUnit": "s", "samplePeriodS": data["samplePeriodS"],
              "samplingRateHz": fs, "frequenciesHz": [k*fs/n for k in range(n//2+1)],
              "frequencyUnit": "Hz", "binSpacingHz": fs/n, "binCount": n//2+1, "sampleCount": n,
              "window": data["window"], "removeMean": data["removeMean"], "filter": data["filter"],
              "windowSum": total, "windowEnergy": energy, "channels": channels}
    return validate_output(result, data)


def compare_results(actual, expected, data):
    """Compare complete validated results with signal-scaled binary64 bounds.

    FFT absolute roundoff scales with history magnitude, N and FFT depth;
    amplitude and PSD inherit that bound through their defining formulas.
    Relative tolerance alone cannot describe error at an empty spectral bin.
    """
    validate_output(actual, data)
    validate_output(expected, data)
    profile = verification_profile()
    atol, rtol = profile["absoluteTolerance"], profile["relativeTolerance"]
    n, fs = len(data["times"]), data["samplingRateHz"]
    maximum = 0.0
    for candidate, oracle in zip(actual["channels"], expected["channels"]):
        scale = max(abs(x) for x in oracle["filteredValues"])
        history_error = 16*math.ulp(1.0)*(1+math.ceil(math.log2(n)))*scale
        fft_error = n*history_error
        for name in ("filteredValues", "fft", "amplitude", "psd"):
            pairs = zip(candidate[name], oracle[name])
            for k,(a,b) in enumerate(pairs):
                endpoint = k == 0 or (n%2 == 0 and k == n//2)
                factor = 1 if endpoint else 2
                if name == "filteredValues":
                    roundoff = history_error
                elif name == "fft":
                    roundoff = fft_error
                elif name == "amplitude":
                    roundoff = factor*math.sqrt(2)*fft_error/expected["windowSum"]
                else:
                    magnitude = math.hypot(*oracle["fft"][k])
                    radius = math.sqrt(2)*fft_error
                    roundoff = factor*(2*magnitude*radius+radius*radius)/(fs*expected["windowEnergy"])
                numbers = zip(a,b) if name == "fft" else [(a,b)]
                for x,y in numbers:
                    error = abs(x-y)
                    maximum = max(maximum, error)
                    if not math.isfinite(error) or not math.isfinite(roundoff) or error > atol + rtol*abs(y) + roundoff:
                        raise ScientificContractError("scientific result disagrees with host reference")
    return {**profile, "maximumAbsoluteError": maximum}


def verify_result(actual, data):
    """Validate every output number against the host; return host-owned profile."""
    expected = reference_result(data, small_n=len(data["times"]) <= 128)
    comparison = compare_results(actual, expected, data)
    return {key: comparison[key] for key in verification_profile()}
