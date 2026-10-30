# v43 static source and package chain

This folder contains the v43 source snapshot, source freeze, OFF install tree, and static finalization scripts. The chain records static evidence only. Every emitted receipt keeps `goPermitted=false` and `productActions.guiLaunched/savePerformed/runPerformed/confirmPerformed=false`; no script in this chain launches the product GUI or performs Save, Run, or Confirm.

`source-preflight-v43.ps1` creates a repeatable source/build-input snapshot. It is a snapshot, not a Release build receipt or source freeze. `release-build-v43.ps1` builds the pinned OFF Release tree and records the executable, link map, CMake cache, log, and before/after source snapshots. `freeze-v43.ps1` accepts the build only after a fresh post-build scan matches the pre-build snapshot and that OFF Release build receipt. It also rehashes the required CTest manifest and CMakeCache. The receipt is useful only when the OFF build receipt and bytes describe the corrected candidate; old OFF build/install pins remain diagnostic evidence and must not be used for a v43 PASS freeze.

The frozen source scope has machine-readable schema `range.pc11-v43-source-scope/v1` and must match the admission verifier's fixed rules exactly:

```json
{
  "includePrefixes": ["src/", "scripts/packaging/", "scripts/benchmark/", "scripts/ci/", "cmake/"],
  "includeBasenamesAnywhere": ["CMakeLists.txt", "CMakePresets.json", "CMakeSettings.json", "vcpkg.json", "vcpkg-configuration.json"],
  "includeExtensionsAnywhere": [".cmake", ".cmake.in"],
  "includeExactPaths": [".github/workflows/windows-regression.yml", "docs/PIEZO_CONTROL_DELIVERY.md", "docs/piezo_control_plan.json"],
  "excludePrefixes": ["build/", ".git/"],
  "enumeration": "git ls-files -co --exclude-standard -z per repo root and recursive submodule; omit gitlink entries; hash all included tracked+nonignored untracked regular files; reject build-like extension outside include rules; reparse/case collision/control chars reject",
  "pathOrder": "UTF8_BYTE_LEXICOGRAPHIC",
  "treeDigest": "SHA256(UTF8(JSON(sourceFiles, sort_keys=true, separators=(',',':'), ensure_ascii=false)))"
}
```

The root and every initialized recursive submodule are enumerated separately with Git's NUL-delimited tracked plus nonignored-untracked list. Gitlink rows are omitted from their parent and their checked-out submodule bytes are scanned in that submodule. Paths use POSIX separators; control characters, case-insensitive collisions, reparse points, non-regular files, unclassified build-like inputs, source-tree changes during hashing, and missing submodules fail closed. Only the repository-root `build/` prefix is excluded; a path such as `src/build/retained.cpp` remains included through the `src/` prefix. Each source row is `{path,sizeBytes,sha256}`. Root rows are checkout-root relative; each `submoduleCommits[].sourceTreeSha256` uses the same rows and digest with paths relative to that submodule. For both digests, rows sort by UTF-8 path bytes and the lower-case SHA256 is calculated over compact UTF-8 JSON with sorted object keys and no BOM. This binds dirty tracked and nonignored untracked bytes as well as commits and gitlinks; `HEAD` alone is insufficient.

`docs/PIEZO_CONTROL_DELIVERY.md` and `docs/piezo_control_plan.json` remain build/source inputs. General narrative and audit evidence are excluded. `docs/PIEZO_DEPENDENCY_PROVENANCE.md` is separately pinned by install/release evidence because it records installation and legal provenance rather than compiled bytes. `build/**` and `.git/**` are excluded from the source tree digest.

The Release build receipt contract is `range.pc11-v43-release-build-receipt/v1`: `status=PASS`, `candidateVersion=v43`, `variant=OFF`, `configuration=Release`, `rootCommit`, `submoduleCommits`, `sourceTreeSha256`, `requiredCTestManifestSha256`, and `cmakeCacheSha256`. It also declares `staticOnly=true`, `goPermitted=false`, and all four product actions false. If present, its `requiredCTestManifestPath` and `cmakeCachePath` must agree with the source-preflight/CMakeCache paths. Freeze compares the same source tree, root/submodule commit pins, and required CTest manifest hash before and after the build.

`package-tree-v43.ps1` reopens the completed admission manifest and inventory report, verifies their externally supplied byte hashes, verifies the installed private Python hash, and invokes the v43 inventory adapter to rehash the complete OFF installation. It writes a package-tree receipt only after the adapter reports `INVENTORY_TREE_MATCH`. The receipt copies dynamic file and directory rows from the schema-v2 report; its `treeDigest` is SHA256 over compact UTF-8 JSON for `{"files":files,"directories":directories}` with sorted object keys, UTF-8-byte-sorted file rows, the report's validated `(casefold,path)` directory order, and lower-case file hashes. Counts come from the validated arrays. No historical v39/v42 file counts, provider counts, or hashes are embedded here.

The package and aggregate static receipts carry `candidateManifestIdentitySha256`, computed over the compact UTF-8 JSON object with sorted keys:

```json
{"candidateVersion":"v43","schema":"range.pc11-fresh-run-v43-admission-candidate/v1","installRoot":"<manifest installBinding.installRoot>","inventoryReportSha256":"<manifest inventory SHA>","sourceFreezeReceiptSha256":"<manifest freeze SHA>","rootCommit":"<manifest root commit>","sourceTreeSha256":"<manifest source tree SHA>","releaseBuildReceiptSha256":"<manifest build SHA>"}
```

This identity deliberately excludes package/static receipt hashes to avoid a cycle. The manifest's `requiredStaticTestIds` and aggregate receipt use these fixed IDs, in this order: `source_preflight`, `source_freeze`, `package_tree`, `module_boundary_build`, `module_boundary_source`, `watchdog_preflight`, `save_predicate_preflight`, `path_shadow`, `root_seal`, `save_predicate`, `watchdog_contract`, `sampler_arguments`, `supervisor`, `worker_handshake`, `native_launch_boundary`, `parser`, `finalize_integrity`, `source_static_tests`, and `root_bootstrap`.

The owned static harness validates the source-freeze and package receipt bindings, verifies producer paths resolve from `tests/` to the prep folder, and runs a Windows PowerShell 5.1 producer-to-admission scope integration test. That test creates a disposable Git fixture with both root `build/` and nested `src/build/` paths, runs the real source preflight, then invokes admission's source re-enumerator and compares the complete row set and digest. It marks `source_preflight`, `source_freeze`, `package_tree`, and `source_static_tests` PASS only when those candidate-bound checks pass. It also runs and binds a separate local 15-gate assessment and a four-gate preflight assessment. The local assessment covers module-boundary helper fixtures and an all-source parser check. The aggregate additionally runs 24 adversarial cases against the four actual producers, checks their test/helper/producer hashes against the frozen post-build source snapshot before execution, holds read-only file handles through the child run, and rechecks the hashes afterward. Only then may it promote the two module-boundary IDs together with `parser`; absent a completed candidate manifest and a full aggregate run, these remain unqualified. The preflight receipt exercises four component fixtures but leaves all four candidate gates `PENDING`. The isolated supervisor/worker fixture similarly proves no product action and does not promote `supervisor`, `worker_handshake`, or `native_launch_boundary` for a release candidate. The aggregate remains `STATIC_INCOMPLETE` and `goPermitted=false` until the complete candidate-bound tests exist.

After a candidate-bound aggregate promotes the two module-boundary IDs, 12 of the 19 fixed IDs still need candidate-bound evidence: watchdog preflight, Save-predicate environment preflight and tests, command/PATH shadow, root seal, watchdog contract, sampler arguments, supervisor, worker handshake, native-launch boundary, finalizer integrity, and root bootstrap. Separate Save-predicate, package-tree scalar/pin, and root-bootstrap fixtures have local evidence but do not promote candidate gates. `save_predicate` is a no-product-action predicate test; actual Save is post-GO product acceptance, so it cannot be a prerequisite for the GO seal. Independent review and a separate root technical decision remain outside this static chain.

Use Windows PowerShell 5.1. Place every generated receipt under the new external audit directory `build/audit-20261014/piezo-delivery/gui-fresh-run-v43-output-audit/`. Each output path is single-use; the scripts refuse to overwrite an existing receipt.

```powershell
$prep = 'D:\YOGA\Documents\Projects\range-fea\scripts\packaging\piezo_gui_static_v43'
$audit = 'D:\YOGA\Documents\Projects\range-fea\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'
$repo = 'D:\YOGA\Documents\Projects\range-fea'
$manifest = '<externally maintained completed candidate-manifest-v43.json>'

# Before the OFF build; record the resulting receipt's external SHA256.
& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
  -File (Join-Path $prep 'source-preflight-v43.ps1') `
  -RepoRoot $repo `
  -RequiredCTestManifestPath (Join-Path $repo 'scripts\ci\required-ctest-tests.json') `
  -AuditRoot $audit `
  -RunId 'source-preflight-v43-before-off-build'

# After configuring a new VS 2026 x64 tree with RANGE_ENABLE_AGENT=OFF and
# RANGE_PRIVATE_PYTHON_DIRECTORY set, produce the real OFF Release receipt.
# The EXE and map must be the outputs of this exact build directory.
& (Join-Path $prep 'release-build-v43.ps1') `
  -PreBuildSourcePreflightPath '<pre-build source-preflight receipt>' `
  -PreBuildSourcePreflightSha256 '<external SHA256>' `
  -BuildDirectory (Join-Path $repo 'build\<fresh-off-release-build>') `
  -CMakeCachePath (Join-Path $repo 'build\<fresh-off-release-build>\CMakeCache.txt') `
  -ExecutablePath (Join-Path $repo 'build\<fresh-off-release-build>\Release\fea.exe') `
  -MapPath (Join-Path $repo 'build\<fresh-off-release-build>\fea\Release\fea-link.map') `
  -BuildLogPath (Join-Path $audit 'off-release-build-v43-001.log') `
  -OutputPath (Join-Path $audit 'off-release-build-v43-001.json')

# After the corrected OFF Release build, install, and fresh schema-v2 inventory:
# Supply the externally recorded pre-build receipt SHA, build receipt SHA, CMakeCache path,
# and a new single-use output path. Freeze performs its own post-build source rescan.
& (Join-Path $prep 'freeze-v43.ps1') `
  -PreBuildSourcePreflightPath '<pre-build source-preflight receipt>' `
  -PreBuildSourcePreflightSha256 '<external SHA256>' `
  -ReleaseBuildReceiptPath '<new OFF Release build receipt>' `
  -ReleaseBuildReceiptSha256 '<external SHA256>' `
  -CMakeCachePath '<new OFF Release CMakeCache.txt>' `
  -OutputPath (Join-Path $audit 'source-freeze-v43-001.json')

# The manifest must already contain the source/build pins and the fresh OFF install/report pins.
& (Join-Path $prep 'package-tree-v43.ps1') `
  -CandidateManifestPath $manifest `
  -CandidateManifestSha256 '<external manifest SHA256>' `
  -OutputPath (Join-Path $audit 'package-tree-v43-001.json')

# Focused standalone checks for the producer/admission scope agreement and harness paths.
& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
  -File (Join-Path $prep 'tests\test-source-scope-v43.ps1') -AuditRoot $audit
& (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
  -File (Join-Path $prep 'tests\test-harness-path-v43.ps1') -AuditRoot $audit

# After updating only the manifest's package-tree receipt path/SHA, run the static harness.
& (Join-Path $prep 'tests\test-static-chain-v43.ps1') `
  -CandidateManifestPath $manifest `
  -OutputPath (Join-Path $audit 'static-tests-v43-001.json')

# After pinning the aggregate static receipt path/SHA in the manifest, finalize.
& (Join-Path $prep 'finalize-v43.ps1') `
  -CandidateManifestPath $manifest `
  -CandidateManifestSha256 '<external manifest SHA256>' `
  -OutputPath (Join-Path $audit 'final-static-v43-001.json')
```

Each step is static evidence only. A PASS source snapshot or package-tree comparison does not assert that the corrected GUI, installed image, Save/Run workflow, or product result was qualified. The finalizer's `STATIC_COMPLETE_NO_GO`, if ever reached, still keeps every product action disabled and grants no product GO.
