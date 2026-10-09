"""Published strict structural schemas; numeric/grid checks also run at execution."""
from . import scientific_contract as contract


def object_schema(properties, required=None):
    return {"type": "object", "properties": properties,
            "required": list(properties) if required is None else required, "additionalProperties": False}


def schemas():
    from .scientific_extensions import GUI, KIND, MANIFEST_SCHEMA
    real = {"type": "number"}
    ident = {"type": "string", "pattern": "^[A-Za-z0-9._:-]{1,128}$"}
    sha = {"type": "string", "pattern": "^[0-9a-f]{64}$"}
    const = lambda value: {"const": value}
    array = lambda item, limit=10000: {"type": "array", "items": item, "maxItems": limit}
    quantities = {"type": "string", "enum": list(contract.UNITS)}
    axis = {"type": "string", "enum": ["x", "y", "z", "scalar"]}
    unit = {"type": "string", "enum": sorted(set(contract.UNITS.values()))}
    source = object_schema({"sourceHash": sha, "artifactId": ident, "channelId": ident})
    channel = object_schema({"id": ident, "quantity": quantities, "axis": axis, "unit": unit,
                            "values": array(real), "source": source}, ["id", "quantity", "axis", "unit", "values"])
    filt = {"oneOf": [object_schema({"kind": const("none")}),
        object_schema({"kind": const("butterworth-lowpass"), "order": const(1), "cutoffHz": {"type": "number", "exclusiveMinimum": 0},
                       "phase": const("causal"), "initialCondition": const("zero")})]}
    fields = {"schema": const(contract.INPUT_SCHEMA), "algorithm": const(contract.ALGORITHM),
        "domain": const("time"), "scalarType": const("real64"), "times": {**array(real), "minItems": 2},
        "timeUnit": const("s"), "samplePeriodS": {"type": "number", "exclusiveMinimum": 0},
        "samplingRateHz": {"type": "number", "exclusiveMinimum": 0},
        "channels": {**array(channel, 4), "minItems": 1},
        "window": {"type": "string", "enum": ["rectangular", "hann"]}, "removeMean": {"type": "boolean"}, "filter": filt}
    output_channel = object_schema({"id": ident, "quantity": quantities, "axis": axis, "unit": unit,
        "amplitudeUnit": unit, "psdUnit": object_schema({"numeratorUnit": unit, "power": const(2), "denominatorUnit": const("Hz")}),
        "filteredValues": array(real), "fft": array({"type": "array", "items": real, "minItems": 2, "maxItems": 2}, 5001),
        "amplitude": array(real, 5001), "psd": array(real, 5001)})
    output = {**{key: fields[key] for key in ("algorithm", "times", "timeUnit", "samplePeriodS", "samplingRateHz", "window", "removeMean", "filter")},
        "schema": const(contract.OUTPUT_SCHEMA), "domain": const("frequency"), "scalarType": const("complex128-pairs"),
        "contractHash": const(contract.CONTRACT_HASH), "inputHash": sha,
        "frequenciesHz": array(real, 5001), "frequencyUnit": const("Hz"), "binSpacingHz": real,
        "binCount": {"type": "integer", "minimum": 2, "maximum": 5001},
        "sampleCount": {"type": "integer", "minimum": 2, "maximum": 10000}, "windowSum": real, "windowEnergy": real,
        "channels": {**array(output_channel, 4), "minItems": 1}}
    manifest = object_schema({"manifestVersion": const(2), "extensionId": ident,
        "version": {"type": "string", "maxLength": 64, "pattern": "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:-[A-Za-z0-9.-]{1,32})?$"},
        "sdkVersion": const(contract.PROTOCOL_VERSION), "kind": const(KIND), "entryPoint": const("extension.py:process"),
        "compatibility": const({"workflowApi": "range.workflow/v1", "inputVersion": 2, "outputVersion": 2}),
        "dependencies": const([{"name": n, "version": contract.DEPENDENCIES[n]} for n in ("numpy", "scipy")]),
        "capabilities": const(["numeric.ndarray.real64", "numeric.fft.rfft", "signal.lowpass1"]),
        "inputSchema": const(contract.INPUT_SCHEMA), "outputSchema": const(contract.OUTPUT_SCHEMA), "gui": const(GUI)})
    return {MANIFEST_SCHEMA: manifest, contract.INPUT_SCHEMA: object_schema(fields), contract.OUTPUT_SCHEMA: object_schema(output)}
