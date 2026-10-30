# Agent-OFF MSVC Release link-map gate (v44)

`verify_off_link_map_v44.py` writes a separate `AGENT_OFF_MSVC_LINK_MAP_GATE` JSON receipt. It does not rewrite or upgrade the v43 inventory report. `linkMapGateStatus: PASS` means only that the externally pinned MSVC map is structurally complete, its parsed Publics and Static rows contain no configured Agent or legacy Cloud AI archive members/symbols, and the map timestamp and PE bytes bind to the OFF Release build/install `fea.exe` records in the pinned v43 report. The receipt always sets `goPermitted: false` and copies the report's existing `releaseQualificationStatus` unchanged.

The parser requires one MSVC section table, one nonempty `Publics by Value` section, a parsed entry point that resolves to a Publics row, and one nonempty `Static symbols` section through end of file. Every row must parse into address, symbol, RVA, flags, and a recognized object/resource/DLL or linker-defined provenance. Duplicate section markers, missing Publics, malformed rows, truncated table tails, unbound timestamps, wrong input names, report ambiguity, and hash mismatches fail closed. Forbidden rows are checked in both Publics and Static symbols; generic `RCloudClient`, `RCloudToolAction`, Cloud, and HTTP symbols remain allowed unless the specific row names Agent, an `rgl_ai_*`/`rai_*` member, or an AI query symbol/member.

All three text evidence inputs require external SHA-256 pins: the complete map, a source identity proof file, and the exact v43 OFF inventory report. The source proof is treated as opaque pinned bytes: use a source snapshot/provenance manifest whose completeness and trust origin are independently governed. Pinning a single source file exercises the hash binding but does not establish a complete source snapshot. The v43 report must be schema version 2, `inventoryStatus: PASS`, `variant: OFF`, and retain a non-PASS/non-GO `releaseQualificationStatus`; its unique `bin/fea.exe` file row and `PROJECT_RELEASE_BUILD` provenance row must match the supplied byte-identical build/install PE files. Map basename and location must be `fea-link.map` under `<buildRoot>/fea/Release`, and its MSVC timestamp must equal both PE COFF timestamps.

The external map hash is important: a structurally plausible map can still be incomplete or fabricated, and neither its filename nor timestamp proves that the linker produced it. The pin lets the operator identify the exact complete map reviewed. This gate does not establish source-to-binary reproducibility, inspect arbitrary executable bytes or runtime plugin loading, or replace release/legal/runtime qualification.

## Historical v43 P1 reproduction

The original OFF map is `build/vs2026-piezo-msvc-openmp-off-v43/fea/Release/fea-link.map` with SHA-256 `f231c73dda854f612d466b5da73f00f69e85063bcaec3f274f6b97cd220a5d5d`. Its `Timestamp is 6acefe1e` matches the COFF timestamp in the byte-identical Release and installed `fea.exe` files. The pinned v43 report is `build/audit-20261014/piezo-delivery/inventory-v43-off-final-v1.json`, SHA-256 `740fae403c4bfa070d3f80b21851a6c9e41d81f9f9d4055a53f91498bc8b505c`. Replaying it produces FAIL because the full map includes old Cloud AI widget/query/request/response members and symbols. The source file pin used for that reproduction is `src/fea/CMakeLists.txt`, SHA-256 `719e4abd87e56bae5900e1231b2f29cf28d2381ec2765569f676af6a8206cd36`; this single-file pin is only a command-path demonstration and is not a full source identity manifest.

## Run

Use pins from the separately reviewed evidence manifest or release record. The following command reproduces the old map failure and writes a new receipt without touching any existing receipt:

```powershell
py -3 .\verify_off_link_map_v44.py `
  --map "D:\YOGA\Documents\Projects\range-fea\build\vs2026-piezo-msvc-openmp-off-v43\fea\Release\fea-link.map" `
  --map-sha256 "f231c73dda854f612d466b5da73f00f69e85063bcaec3f274f6b97cd220a5d5d" `
  --build-exe "D:\YOGA\Documents\Projects\range-fea\build\vs2026-piezo-msvc-openmp-off-v43\Release\fea.exe" `
  --install-exe "D:\YOGA\Documents\Projects\range-fea\build\audit-20261014\piezo-delivery\install-off-v43\bin\fea.exe" `
  --source-proof "D:\YOGA\Documents\Projects\range-fea\src\fea\CMakeLists.txt" `
  --source-sha256 "719e4abd87e56bae5900e1231b2f29cf28d2381ec2765569f676af6a8206cd36" `
  --report "D:\YOGA\Documents\Projects\range-fea\build\audit-20261014\piezo-delivery\inventory-v43-off-final-v1.json" `
  --report-sha256 "740fae403c4bfa070d3f80b21851a6c9e41d81f9f9d4055a53f91498bc8b505c" `
  --output "D:\YOGA\Documents\Projects\range-fea\build\audit-20261014\piezo-delivery\off-link-map-v44-old-fail.json"
```

For a repaired v44 OFF build, first install that same Release build, run `inventory_v43.py` against the clean install to produce a new OFF report, and independently record the new source proof, report, and `fea-link.map` hashes. Then substitute the new build root paths and three external pins above and select a new output filename. The script does not build, install, or produce those external pins.

Run synthetic parser and identity cases from this directory:

```powershell
py -3 -m unittest -v test_verify_off_link_map_v44.py
```
