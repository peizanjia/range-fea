# Control material and device profile foundation (CW-04 partial)

This catalog supplies reusable, validated SI templates for future GUI, SDK and
native adapter use. It does **not** implement FE assembly, solver dispatch,
electrical boundary conditions, model attachment, hardware acquisition or CW-04
acceptance. No existing service, build configuration or native material type is
changed. The bundled data is independently synthetic demonstration data, not
PZT-5H data, a vendor datasheet or measurements of a real device. Users must
replace or calibrate it before making physical predictions.

## Python interface

```python
from range_workflow.control_profiles import (
    load_catalog, validate_material, validate_device, catalog_digest,
)
catalog = load_catalog()  # package-relative data/control_materials.json
material = validate_material(catalog["materials"][1])
device = validate_device(catalog["devices"][0], catalog["materials"])
fingerprint = catalog_digest(catalog)
```

`load_catalog(path=None)` reads UTF-8 JSON and returns fresh canonical dictionaries
and lists. `validate_material(profile)` and `validate_device(profile, materials)`
also return fresh canonical values and do not mutate their input. `materials` is
an iterable of material profiles, not an ID-to-material mapping. Invalid profiles
raise `ValueError`; file access failures retain normal filesystem exceptions.
No dependency beyond Python's standard library is needed.

`catalog_digest(catalog)` hashes UTF-8 JSON with SHA256, sorted object keys,
compact separators, no ASCII escaping and no nonfinite values. Array order is
significant. Hash the output of `load_catalog` to fingerprint canonical profiles;
the digest function itself is a serialization fingerprint, not a schema validator.
It includes versions, units, state, provenance and metadata. Integer and float
spellings are normalized for physical parameters by the validators; external
consumers must hash the same canonical JSON representation rather than arbitrary
input bytes. This digest is not a solver qualification or calibration certificate.

## Shared contracts

The catalog has exactly `schema`, `version`, `metadata`, `materials`, `devices`.
Its schema is `range.control-catalog/v1`; version is a nonempty revision string.
Metadata has exactly three nonempty strings: `purpose`, `assumptions`,
`qualification`. Material and device IDs must be globally unique within a catalog.

Every profile has `schema`, `id`, `name`, `version`, `units`, `provenance`, `type`.
ID, name and version are nonempty strings. `units` must be `SI`. Provenance has
exactly `kind` (`example`, `datasheet`, or `calibrated`), nonempty `source`, and
nonempty `note`. Provenance records the author's claim; the validator does not
authenticate a source or verify measurements. Unknown fields, missing fields,
unknown schemas, duplicate JSON keys at any depth, NaN/Infinity, booleans used as
numbers, and unsupported unit systems are rejected. No implicit unit conversion
is performed. Nonempty text fields must encode as UTF-8; isolated Unicode
surrogates are rejected during validation. Convert units explicitly before
constructing an SI profile.

## Materials

Schema: `range.control-material/v1`. Material profiles contain `properties`.

| Type | Required properties | Checks |
| --- | --- | --- |
| `isotropic-elastic` | `E_Pa`, `nu`, `rho_kg_m3` | E and density positive; -1 < nu < 0.5 |
| `piezoelectric`, `stress-charge` | `formulation`, `rho_kg_m3`, `cE_Pa`, `e_C_m2`, `epsilonS_F_m` | density positive, 6x6 cE and 3x3 epsilonS symmetric positive definite, e 3x6 |
| `piezoelectric`, `strain-charge` | `formulation`, `rho_kg_m3`, `cE_Pa`, `d_C_N`, `epsilonT_F_m` | d 3x6, epsilonT 3x3 symmetric positive definite; converted epsilonS must also be positive definite |

Piezoelectric materials additionally require exactly this `tensorConvention`:

```json
{"voigtOrder":["xx","yy","zz","yz","xz","xy"],"shear":"engineering","frame":"material"}
```

Mechanical strain vector is `[eps_xx, eps_yy, eps_zz, 2eps_yz, 2eps_xz,
2eps_xy]`; stress vector has unscaled shear stresses. Electric components are
`[x,y,z]` in the same material frame. `cE_Pa` is stiffness at constant electric
field (Pa), **not compliance**. Absolute permittivity is in F/m; relative
permittivity cannot be substituted or tagged as absolute. `epsilonRelative` and
mixed units are rejected as fields. The validator cannot detect a falsely labeled
physical value, so provenance and explicit SI conversion remain necessary.
The existing native `RMaterialProperty::RelativePermittivity` scalar is therefore
not a direct mapping for these absolute 3x3 tensors. A future adapter must perform
an explicit, qualified conversion rather than equating the names.

Stress-charge canonical equations are:

```text
stress = cE * engineering_strain - transpose(e) * electric_field
electric_displacement = e * engineering_strain + epsilonS * electric_field
e = d * cE
epsilonS = epsilonT - d * cE * transpose(d)
```

The conversion changes constitutive representation, not material axes or poling.
All canonical piezoelectric output uses `stress-charge`. Matrix elements must be
finite. Symmetry permits relative roundoff of 1e-12 and averages matching entries;
there is no absolute tolerance that hides small dielectric asymmetry. Positive
definiteness is checked with standard-library Cholesky with strictly positive
pivots. Numerically unrepresentable conversion/factorization is rejected. No
condition-number guarantee or FE stability claim follows from these checks.
Tensor rotations, electrode polarity and geometry transforms belong to a future
native adapter and must be explicit there.
The symmetric conversion term d*cE*transpose(d) is accumulated with `math.fsum`
for one triangle and mirrored, preserving the constitutive symmetry even near
off-diagonal cancellation. This does not relax symmetry checks on input tensors.

## Device templates

Schema: `range.control-device/v1`. Devices additionally contain `state` and
`parameters`. The only accepted state is `ready-for-binding`: no DOF, electrical
port, mesh element, anchor or sensor location has yet been bound. All direction
vectors below are three-component unit vectors in the material frame, with unit
length tolerance 1e-12. They are retained unchanged, without rotation.

| Type / mode | Exact parameters | Material reference |
| --- | --- | --- |
| `piezo-rod` / `material-based` | `mode`, positive `length_m`, `area_m2`, `electrodeSeparation_m`, `axis`, `electricFieldDirection` | required top-level `materialId` referencing a piezoelectric material |
| `piezo-rod` / `calibrated-effective` | `mode`, positive `k_N_m`, `capacitanceClamped_F`, `mass_kg`, finite signed `freeStroke_m_V` | forbidden; effective values cannot be mixed with geometry or tensors |
| `piezo-patch` | positive `length_m`, `width_m`, `thickness_m`, `side` (`positive` or `negative`), `polingDirection`, `bond` = `ideal` | required top-level `materialId` referencing a piezoelectric material |
| `cable-length-actuator` | `massModel` = `fixed-mass-equivalent`, positive `fixedMass_kg`, `referenceLength_m`, `minLength_m`, `maxLength_m` | forbidden |
| `virtual-sensor` | `quantity`, `unit`, `location` = `bind-later` | forbidden |

An active rod can later bind to an axial member of a ring truss; it is not a patch.
Its axis and field direction can differ. Geometry and material data are kept
separate; no effective k, stroke, mass or capacitance is inferred here.
`calibrated-effective` names an effective parameter model; actual measurement
status is given by provenance, not by that mode name. `k_N_m` is the positive
short-circuit (voltage held at zero) mechanical stiffness. `freeStroke_m_V` is
the signed free axial extension per applied voltage at zero axial force, paired
with that stiffness. It encodes the voltage/extension sign convention and may be
zero. `capacitanceClamped_F` is measured at fixed mechanical extension, not free
mechanical motion. In a future reciprocal single-port adapter, the corresponding
coupling would be theta = k * freeStroke, and free capacitance would be
capacitanceClamped + k * freeStroke^2. No such kernel is implemented here.
Datasheet free capacitance must not be supplied as clamped capacitance: convert
it explicitly with compatible stiffness/coupling data before constructing this
profile. Generic `capacitance_F`, free-capacitance fields and other conditions
are rejected. These two rod modes cannot coexist in one profile.

A patch expresses an ideal bonded beam/plate face assumption; it does not model
adhesive compliance, delamination or thermal effects. Face orientation and spatial
placement must be bound later. Poling direction is descriptive input in the
material frame; it does not automatically flip or rotate the referenced tensor.

A cable template represents a fixed mass equivalent length actuator between
future anchors. Its reference length lies within a strictly nonzero positive
length interval. This does not model a physical spool, winding inertia, variable
mass, anchor motion or pretension equilibrium. Anchor or nodal attachment is a
future binding decision.

Virtual sensor quantities and exact SI units are `displacement`/`m`,
`velocity`/`m/s`, `acceleration`/`m/s^2`, `strain`/`1`, `tension`/`N`,
`charge`/`C`, `voltage`/`V`. They carry no mass, hardware DAQ capability, channel,
noise model or automatic electrical availability. Components, sample timing and
locations are binding concerns. Charge/voltage templates do not imply that a
chosen native solver exposes those quantities.

The default catalog includes one elastic and one piezoelectric material, one rod,
one ideal patch, one fixed mass cable and seven virtual sensors. Real datasheet
curation and measured effective rods are later work. Native consumption still
requires explicit material/geometry mapping, ownership of mass contributions,
port/DOF binding and solver qualification. These remain outside this partial
CW-04 catalog foundation.

## Focused validation

From the repository/worktree root:

```powershell
python src/range-workflow/tests/test_control_profiles.py
```

The 24 unittest methods cover a hand-computed nonzero coupling conversion,
dielectric stability after conversion, matrix shape/finiteness/symmetry,
strict fields/schemas/JSON, SI and tensor conventions, ID/reference integrity,
mutually exclusive rod models, geometric positivity, patch and cable assumptions,
virtual sensor isolation, digest sensitivity and canonical ownership/idempotence.
These are catalog contract checks; they do not establish native solver support.
Acceleration uses exactly `m/s^2`, matching the native quantity contract;
the alternate spelling `m/s2` is rejected rather than treated as an alias.
