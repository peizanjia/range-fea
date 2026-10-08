# Native document CLI and Python SDK

`range-model-command` is a file-scoped adapter over `RModelCommandService` and `RModelCommandSession`. It reads a native `.ram`/`.rbm` model or a native LINK chain, validates one bounded operation, and for an explicit execute request publishes a separate native output file. The command never attaches to or edits a live GUI document, never reports GUI job state, and never starts a solver job. It cannot tell whether a GUI has the same file open. The separate GUI host, Agent tools, and native solver job contract is documented in [Native Model Operations](NATIVE_MODEL_OPERATIONS.md).

The CLI prints one JSON object on stdout. Diagnostics go to stderr. A rejected request has `status: "rejected"` and a stable service or CLI `code`; validation and stale-source failures exit with code 2. A successful preview exits 0 without writing a model. A successful output publication exits 0 after atomic file replacement; an audit failure after publication exits 3 and reports that the file was already written. The input source is never the publication destination.

## CLI operations

```text
range-model-command --input MODEL --query
range-model-command --input MODEL --capabilities
range-model-command --input MODEL --context [--page REQUEST.json]
range-model-command --input MODEL --targets [--page REQUEST.json]
range-model-command --input MODEL --parameters REQUEST.json --dry-run
range-model-command --input MODEL --command COMMAND_ID --parameters REQUEST.json --dry-run
range-model-command --input MODEL --command COMMAND_ID --parameters REQUEST.json --output OUTPUT
```

Without `--command`, a parameter request retains the original modal operation, `analysis.modal.configure`. With `--command`, the CLI sends the named operation to the shared application service. `--dry-run` means preview. Omitting it means execute and requires a distinct `--output` path. Existing `--journal` and `--reconcile` behavior remains available for command publication and recovery.

The parameter file is a strict JSON object with exactly these top-level fields:

```json
{"sourceSha256":"64 lowercase hex digits from --query","parameters":{}}
```

`sourceSha256` is the raw SHA256 of the resolved terminal model file. The CLI resolves MODEL/LINK source chains with the project catalog helper, locks and hashes every source artifact, and verifies the complete binding before and after reading and before publication. A changed source, stale SHA256, unknown argument, unsupported operation, or invalid unit is rejected before output publication. A LINK file and its resolved model are both part of this source check.

For `--context` and `--targets`, `--page` is a JSON object using the live Core page fields `limit`, `offset`, `cursor`, and/or `labelQuery` as allowed by the current page rules. `--capabilities` returns the same command descriptions used by validation. Context and target pages use a CLI-owned identity derived from the canonical input path and terminal source SHA256, with the source SHA256 as the page revision. This lets a cursor continue in a later CLI process while the source bytes and path remain the same. A changed source rejects the cursor as stale. Callers cannot choose or impersonate a GUI source identity.

`--targets` label filtering is read-only. The service returns `targets`, `candidateIds`, `ambiguous`, and pagination metadata; the CLI and SDK never select a candidate from a label. Commands that edit an entity require an explicit physical target ID such as `g:18`.

## Supported command shapes

Use the current `--capabilities` result as the authority for supported commands, method lists, required fields, optional defaults, and units. The native service performs the final exact-field and value validation.

| Command | Parameters and units |
| --- | --- |
| `analysis.modal.configure` | `method`, `iterations`, `modes`, `tolerance`, `unit`; `method` is advertised as `dominant` or `multiple`, and the tolerance unit is `1`. |
| `analysis.structural_dynamics.configure` | Full configuration including `enabled`, `method`, `timeStep`, `unitSI: true`, `steps`, `outputFrequency`, `rayleighAlpha`, `rayleighBeta`, `rhoInfinity`, `newmarkGamma`, `newmarkBeta`, `modalDampingRatios`. SI time and damping units are described in capabilities. |
| `entity.material.set` | `targetId`, `name`, `youngModulusPa`, `densityKgM3`, `poissonRatio`, `thermalExpansionPerK`, and optional `referenceTemperatureK` (default 293.15 K). The edit writes a constant solid with four SI properties. Tabulated versions of those edited properties are rejected. |
| `entity.boundary.set` | `targetId`, `kind`, three global XYZ `values`, and `unit`; optional `timeS` defaults to 0. Force uses N and requires a one-node Point group on a Stress-only model. Displacement uses m and supports the service-advertised group types. A `null` axis disables that component. |

Physical group IDs use the native `g:<uint>` form. Current v1.3.18 models persist those IDs across serialization. Legacy v1.3.17 groups are migrated deterministically when read; use IDs returned by the current `--targets` response rather than reconstructing IDs from local array positions.

## Python client

Import the wrapper from `range_workflow.native_document_client`. It invokes the actual CLI using a subprocess argument list and writes command JSON to a temporary request file. It does not invoke a shell.

```python
from range_workflow.native_document_client import NativeDocumentClient

client = NativeDocumentClient("range-model-command", "input.ram")
source = client.query()                 # includes sourceSha256 and an ephemeral modelId
capabilities = client.capabilities()
page = client.targets({"limit": 50, "labelQuery": "Beam"})
context = client.context()

preview = client.set_material(
    target_id="g:18", name="Steel",
    young_modulus_pa=2.0e11, density_kg_m3=7850.0,
    poisson_ratio=0.3, thermal_expansion_per_k=1.2e-5)
# The caller decides whether to issue this explicit file publication call.
published = client.execute(preview, "configured.ram")
```

`configure_modal`, `configure_structural_dynamics`, `set_material`, and `set_boundary` read the live capabilities and, where applicable, target pages before returning an immutable `NativeCommandPreview`. A preview binds the source SHA256, command ID, normalized parameters, a digest, and the service fingerprint when that operation returns one. The fingerprint is specific to that preview call and is not a cross-process model identity. `execute(preview, output_path, journal_path=None)` accepts only a preview created by that same client, checks that the source SHA256 still matches, and sends the frozen normalized request back through CLI validation. These methods do not execute on their own. The SDK has no approval method or model-provided approval token; the application that owns the client must decide whether to call `execute`.

The service returns session `modelId` and `revision` values for mutation requests. They belong to one short-lived CLI process and must not be reused as cross-process or GUI identities. The SDK binds previews across processes with the raw source SHA256 and stable physical target IDs. Read-only target pagination has its separate CLI-owned path-plus-source identity so target cursors can continue across CLI processes. A target cursor is rejected when its source changes.

## Scope and limits

This interface exposes only the capabilities returned by the shared application service. It does not expose arbitrary Qt calls, native object reflection, Python evaluation, shell execution, material-table conversion, boundary-condition families outside the advertised schema, or solver controls. A `preview` proves only that the current native service accepts the proposed configuration. A `published` response proves that a new native file was written; it does not prove numerical solvability, run a solver, or synchronize an already open GUI document. Solver process and job ownership remain with their host application.
