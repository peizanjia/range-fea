"""Strict SI control profile interchange; validation is not solver qualification."""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

VOIGT = ["xx", "yy", "zz", "yz", "xz", "xy"]


def _fields(value, required, optional=()):
    if not isinstance(value, dict):
        raise ValueError("Expected object")
    if set(value) - set(required) - set(optional) or set(required) - set(value):
        raise ValueError("Unknown or missing fields")


def _text(value):
    if not isinstance(value, str) or not value.strip():
        raise ValueError("Expected nonempty string")
    try:
        value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError("Expected valid UTF-8 text without isolated surrogates") from exc
    return value


def _number(value, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("Expected finite number, not bool")
    try:
        value = float(value)
    except (OverflowError, ValueError) as exc:
        raise ValueError("Expected finite number") from exc
    if not math.isfinite(value) or (positive and value <= 0):
        raise ValueError("Expected finite positive number" if positive else "Expected finite number")
    return 0.0 if value == 0 else value


def _matrix(value, rows, cols, symmetric=False, pd=False):
    if not isinstance(value, list) or len(value) != rows:
        raise ValueError("Incorrect matrix dimensions")
    result = []
    for row in value:
        if not isinstance(row, list) or len(row) != cols:
            raise ValueError("Incorrect matrix dimensions")
        result.append([_number(x) for x in row])
    if symmetric:
        for i in range(rows):
            for j in range(i):
                if not math.isclose(result[i][j], result[j][i], rel_tol=1e-12, abs_tol=0):
                    raise ValueError("Matrix must be symmetric")
                result[i][j] = result[j][i] = (result[i][j] + result[j][i]) / 2
    if pd:
        lower = [[0.0] * rows for _ in range(rows)]
        for i in range(rows):
            for j in range(i + 1):
                residual = result[i][j] - sum(lower[i][k] * lower[j][k] for k in range(j))
                if not math.isfinite(residual) or (i == j and residual <= 0):
                    raise ValueError("Matrix must be positive definite")
                lower[i][j] = math.sqrt(residual) if i == j else residual / lower[j][j]
                if not math.isfinite(lower[i][j]):
                    raise ValueError("Unrepresentable Cholesky factor")
    return result


def _base(profile, schema):
    for key in ("id", "name", "version"):
        _text(profile[key])
    if profile["schema"] != schema or profile["units"] != "SI":
        raise ValueError("Unsupported schema or units; only explicit SI is supported")
    provenance = profile["provenance"]
    _fields(provenance, ("kind", "source", "note"))
    if provenance["kind"] not in ("example", "datasheet", "calibrated"):
        raise ValueError("Unknown provenance kind")
    for key in ("source", "note"):
        _text(provenance[key])
    return {key: profile[key] for key in ("schema", "id", "name", "version", "units")} | {
        "provenance": dict(provenance)}


def validate_material(profile):
    """Return a fresh canonical SI material; strain-charge becomes stress-charge."""
    _fields(profile, ("schema", "id", "name", "version", "units", "provenance", "type", "properties"),
            ("tensorConvention",))
    result = _base(profile, "range.control-material/v1")
    p = profile["properties"]
    kind = profile["type"]
    if kind == "isotropic-elastic":
        if "tensorConvention" in profile:
            raise ValueError("Elastic scalar material has no tensorConvention")
        _fields(p, ("E_Pa", "nu", "rho_kg_m3"))
        nu = _number(p["nu"])
        if not -1 < nu < 0.5:
            raise ValueError("Poisson ratio must lie strictly between -1 and 0.5")
        properties = {"E_Pa": _number(p["E_Pa"], True), "nu": nu,
                      "rho_kg_m3": _number(p["rho_kg_m3"], True)}
    elif kind == "piezoelectric":
        convention = profile.get("tensorConvention")
        _fields(convention, ("voigtOrder", "shear", "frame"))
        if convention != {"voigtOrder": VOIGT, "shear": "engineering", "frame": "material"}:
            raise ValueError("Unsupported tensor convention")
        formulation = p.get("formulation") if isinstance(p, dict) else None
        if formulation == "stress-charge":
            _fields(p, ("formulation", "rho_kg_m3", "cE_Pa", "e_C_m2", "epsilonS_F_m"))
        elif formulation == "strain-charge":
            _fields(p, ("formulation", "rho_kg_m3", "cE_Pa", "d_C_N", "epsilonT_F_m"))
        else:
            raise ValueError("Unsupported piezoelectric formulation")
        stiffness = _matrix(p["cE_Pa"], 6, 6, True, True)
        if formulation == "stress-charge":
            coupling = _matrix(p["e_C_m2"], 3, 6)
            dielectric = _matrix(p["epsilonS_F_m"], 3, 3, True, True)
        else:
            d = _matrix(p["d_C_N"], 3, 6)
            epsilon_t = _matrix(p["epsilonT_F_m"], 3, 3, True, True)
            try:
                coupling = [[math.fsum(d[i][k] * stiffness[k][j] for k in range(6))
                             for j in range(6)] for i in range(3)]
                coupling = _matrix(coupling, 3, 6)
                # d*cE*dT is symmetric by construction. Compute one triangle:
                # separate accumulation of both halves can amplify roundoff
                # into a spurious asymmetry when epsilonS entries nearly cancel.
                dielectric = [[0.0] * 3 for _ in range(3)]
                for i in range(3):
                    for j in range(i + 1):
                        correction = math.fsum(coupling[i][k] * d[j][k] for k in range(6))
                        dielectric[i][j] = dielectric[j][i] = epsilon_t[i][j] - correction
            except (OverflowError, ValueError) as exc:
                raise ValueError("Unrepresentable piezoelectric conversion") from exc
            dielectric = _matrix(dielectric, 3, 3, True, True)
        properties = {"formulation": "stress-charge", "rho_kg_m3": _number(p["rho_kg_m3"], True),
                      "cE_Pa": stiffness, "e_C_m2": coupling, "epsilonS_F_m": dielectric}
        result["tensorConvention"] = {"voigtOrder": list(VOIGT), "shear": "engineering", "frame": "material"}
    else:
        raise ValueError("Unsupported material type")
    return result | {"type": kind, "properties": properties}


def _direction(value):
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError("Direction must have three components")
    direction = [_number(x) for x in value]
    if not math.isclose(math.hypot(*direction), 1, rel_tol=1e-12, abs_tol=1e-12):
        raise ValueError("Direction must be a unit vector in material frame")
    return direction


SENSOR_UNITS = {"displacement": "m", "velocity": "m/s", "acceleration": "m/s^2",
                "strain": "1", "tension": "N", "charge": "C", "voltage": "V"}


def validate_device(profile, materials):
    """Validate a ready-for-binding template, without adding DOFs or sensor mass.

    materials is an iterable of material profiles (canonical or source form).
    """
    _fields(profile, ("schema", "id", "name", "version", "units", "provenance", "type", "state", "parameters"),
            ("materialId",))
    result = _base(profile, "range.control-device/v1")
    if profile["state"] != "ready-for-binding":
        raise ValueError("Only unbound ready-for-binding templates are supported")
    known = {}
    for material in materials:
        canonical = validate_material(material)
        if canonical["id"] in known:
            raise ValueError("Duplicate material id")
        known[canonical["id"]] = canonical
    kind, p = profile["type"], profile["parameters"]
    if not isinstance(p, dict):
        raise ValueError("Expected parameters object")
    mode = p.get("mode")
    material_based = kind == "piezo-patch" or (kind == "piezo-rod" and mode == "material-based")
    if material_based:
        ref = _text(profile.get("materialId"))
        if ref not in known or known[ref]["type"] != "piezoelectric":
            raise ValueError("Device requires an existing piezoelectric material")
        result["materialId"] = ref
    elif "materialId" in profile:
        raise ValueError("This device mode cannot carry materialId")
    if kind == "piezo-rod" and mode == "material-based":
        _fields(p, ("mode", "length_m", "area_m2", "electrodeSeparation_m", "axis", "electricFieldDirection"))
        parameters = {key: _number(p[key], True) for key in ("length_m", "area_m2", "electrodeSeparation_m")}
        parameters |= {key: _direction(p[key]) for key in ("axis", "electricFieldDirection")}
        parameters["mode"] = mode
    elif kind == "piezo-rod" and mode == "calibrated-effective":
        _fields(p, ("mode", "k_N_m", "freeStroke_m_V", "capacitanceClamped_F", "mass_kg"))
        parameters = {key: _number(p[key], True) for key in ("k_N_m", "capacitanceClamped_F", "mass_kg")}
        parameters |= {"mode": mode, "freeStroke_m_V": _number(p["freeStroke_m_V"])}
    elif kind == "piezo-patch":
        _fields(p, ("length_m", "width_m", "thickness_m", "side", "polingDirection", "bond"))
        if p["side"] not in ("positive", "negative") or p["bond"] != "ideal":
            raise ValueError("Patch requires a positive/negative face and explicit ideal bond")
        parameters = {key: _number(p[key], True) for key in ("length_m", "width_m", "thickness_m")}
        parameters |= {"side": p["side"], "bond": "ideal", "polingDirection": _direction(p["polingDirection"])}
    elif kind == "cable-length-actuator":
        _fields(p, ("massModel", "fixedMass_kg", "referenceLength_m", "minLength_m", "maxLength_m"))
        if p["massModel"] != "fixed-mass-equivalent":
            raise ValueError("Only fixed mass equivalent cable is supported")
        parameters = {key: _number(p[key], True) for key in ("fixedMass_kg", "referenceLength_m", "minLength_m", "maxLength_m")}
        if not parameters["minLength_m"] <= parameters["referenceLength_m"] <= parameters["maxLength_m"] or parameters["minLength_m"] == parameters["maxLength_m"]:
            raise ValueError("Reference length must lie within nonzero travel interval")
        parameters["massModel"] = p["massModel"]
    elif kind == "virtual-sensor":
        _fields(p, ("quantity", "unit", "location"))
        if not isinstance(p["quantity"], str) or SENSOR_UNITS.get(p["quantity"]) != p["unit"] or p["location"] != "bind-later":
            raise ValueError("Unsupported sensor quantity, unit or location")
        parameters = dict(p)
    else:
        raise ValueError("Unsupported device type or mode")
    return result | {"type": kind, "state": "ready-for-binding", "parameters": parameters}


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError("Nonfinite JSON constant: " + value)


def load_catalog(path=None):
    """Load strict JSON and return independently owned canonical profiles."""
    path = Path(path) if path is not None else Path(__file__).parent / "data" / "control_materials.json"
    catalog = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object,
                         parse_constant=_reject_constant)
    _fields(catalog, ("schema", "version", "metadata", "materials", "devices"))
    if catalog["schema"] != "range.control-catalog/v1":
        raise ValueError("Unsupported catalog schema")
    _text(catalog["version"])
    _fields(catalog["metadata"], ("purpose", "assumptions", "qualification"))
    for value in catalog["metadata"].values():
        _text(value)
    if not isinstance(catalog["materials"], list) or not isinstance(catalog["devices"], list):
        raise ValueError("Materials and devices must be arrays")
    materials = [validate_material(p) for p in catalog["materials"]]
    ids = [p["id"] for p in materials]
    if len(set(ids)) != len(ids):
        raise ValueError("Duplicate material id")
    devices = [validate_device(p, materials) for p in catalog["devices"]]
    ids += [p["id"] for p in devices]
    if len(set(ids)) != len(ids):
        raise ValueError("Catalog ids must be globally unique")
    return {"schema": catalog["schema"], "version": catalog["version"],
            "metadata": dict(catalog["metadata"]), "materials": materials, "devices": devices}


def catalog_digest(catalog):
    """SHA256 of strict sorted JSON; call load_catalog first for schema validation.

    Array order is significant. Includes units, state, provenance and metadata.
    This fingerprints data, not numerical qualification or solver capability.
    """
    encoded = json.dumps(catalog, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                         allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()
