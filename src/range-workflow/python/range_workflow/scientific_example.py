"""Independently authored example candidate for the frozen spectral/filter v2 API."""
import hashlib
import json
import numpy as np
from scipy.signal import butter, sosfilt

CONTRACT_HASH = "0708308690808434e9552c2f96e718af3c28fa15ce63fae2f6b04daf19f0565f"


def process(data):
    fs, n = data["samplingRateHz"], len(data["times"])
    weights = np.ones(n) if data["window"] == "rectangular" else .5-.5*np.cos(2*np.pi*np.arange(n)/n)
    total, energy = float(np.sum(weights)), float(np.sum(weights*weights))
    bins = np.arange(n//2+1)
    factor = np.full(len(bins), 2.)
    factor[0] = 1.
    if n % 2 == 0:
        factor[-1] = 1.
    channels = []
    for channel in data["channels"]:
        values = np.array(channel["values"], dtype=np.float64)
        if data["filter"]["kind"] == "butterworth-lowpass":
            values = sosfilt(butter(1, data["filter"]["cutoffHz"], fs=fs, output="sos"), values)
        prepared = values-values.mean() if data["removeMean"] else values
        transformed = np.fft.rfft(prepared*weights)
        transformed[0] = transformed[0].real
        if n % 2 == 0:
            transformed[-1] = transformed[-1].real
        magnitude = np.abs(transformed)
        channels.append({**{key: channel[key] for key in ("id", "quantity", "axis", "unit")},
            "amplitudeUnit": channel["unit"],
            "psdUnit": {"numeratorUnit": channel["unit"], "power": 2, "denominatorUnit": "Hz"},
            "filteredValues": values.tolist(), "fft": np.column_stack((transformed.real, transformed.imag)).tolist(),
            "amplitude": (factor*magnitude/total).tolist(), "psd": (factor*magnitude*magnitude/(fs*energy)).tolist()})
    raw = json.dumps(data, ensure_ascii=False, sort_keys=True, allow_nan=False, separators=(",", ":")).encode()
    return {"schema": "range.extension/v2/spectral-result", "algorithm": "spectral-filter/v1",
        "contractHash": CONTRACT_HASH, "inputHash": hashlib.sha256(raw).hexdigest(),
        "domain": "frequency", "scalarType": "complex128-pairs", "times": data["times"], "timeUnit": "s",
        "samplePeriodS": data["samplePeriodS"], "samplingRateHz": fs, "frequenciesHz": (bins*fs/n).tolist(),
        "frequencyUnit": "Hz", "binSpacingHz": fs/n, "binCount": len(bins), "sampleCount": n,
        "window": data["window"], "removeMean": data["removeMean"], "filter": data["filter"],
        "windowSum": total, "windowEnergy": energy, "channels": channels}
