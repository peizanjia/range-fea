# v43 static GUI candidate preparation scaffold

This isolated scaffold carries forward the v39 candidate's staged boundary: prepare and verify static evidence first, keep product actions disabled, and require independent review plus a separate root technical decision before any GUI action. It is not frozen and it does not admit a GUI launch.

The v39 candidate is a diagnostic package tied to an older installed tree, inventory-v1 records (`prefix`, `bytes`), fixed inventory/provider counts, and installed executable hashes. Its admission code cannot be reused as the v43 byte authority. This scaffold reads the current v43 schema-v2 report contract produced by `scripts/packaging/inventory_v43.py`, which requires `directories` as well as `files`; earlier schema-v2 reports without that field are historical evidence and are rejected. The adapter requires `variant: "OFF"`, the exact `installRoot`, `files[].sizeBytes`, a matching dynamic `fileCount`, a bounded canonical sorted directory list (including empty directories and excluding the install root), all 18 required producer check IDs exactly once and PASS, and an externally supplied report SHA256. It makes no fixed file-count assumption.

`candidate-manifest-v43.template.json` keeps the installed paths relative and the unavailable bytes/pins explicit. Replace its pending values only after the fresh OFF Release install and a new-format inventory report exist. Keep the inventory report outside the install root; the inventory producer and this adapter both enforce that boundary. Derive the final file count from validated report rows. Write the new report to a new filename, preserving any earlier ON schema-v2 receipt unchanged.

After the live OFF install and report are available, run the tree review with the independently recorded report SHA256:

```powershell
py -3 -B .\inventory_adapter_v43.py `
  --install-root "D:\path\to\fresh-v43-off-install" `
  --inventory-report "D:\path\to\audit\inventory-v43-off-v2-directories.json" `
  --inventory-sha256 "<externally-recorded-report-sha256>" `
  --variant OFF
```

The public review API is `verify_install_tree(report_path, install_root, external_sha256, variant="OFF")`: it loads and pins the report itself before calling the private snapshot scanner. `adapt_inventory_report` does not accept caller-provided report hashes, and snapshots are not a public tree-review input. The command re-enumerates and hashes the current install tree, then compares every relative file path, byte size, SHA256, and directory path with the pinned report. The explicit directory list preserves installed empty directories and allows the adapter to reject added or removed empty directories accurately. It rejects case-only file or directory changes and case-insensitive collisions, special files, symlinks, junctions and other reparse points, path escapes, and files that change while being hashed. Traversal is bounded by the same scale as the v43 producer: 50,000 files, 100,000 entries, 128 directory levels, 20 GiB total, and 10 GiB per file. A successful command prints `INVENTORY_TREE_MATCH`; that is a byte-tree comparison result only, with `guiAdmissionEvaluated: false`, `goPermitted: false`, and all product actions false.

The adapter and its tests are one part of the current v43 work. The same folder now also contains v43-specific source preflight/freeze, package-tree and finalizer producers, local static-gate assessments, and isolated supervisor/worker fixture code. The fixture receipt does not admit a product launch. The final OFF install and independently pinned source/build/map evidence are still absent; the candidate manifest therefore retains pending pins and all product actions remain disabled. Historical v39/v42 counts, hashes, and installed-tree authority must not be transplanted.

Run the bounded synthetic adapter tests from this directory with:

```powershell
py -3 -B -m unittest -v tests.test_inventory_adapter_v43 tests.test_install_tree_review_v43
```

These tests exercise schema/variant/root/count/row/hash rejection, required directory-field validation and ordering, exact/unique/all-PASS producer check IDs, external report pinning, public API bypass rejection, exact synthetic-tree matching with an empty directory, and rejection of missing/extra/changed/case-mismatched files or directories and reparse entries using temporary files. They do not run `inventory_v43.py` against an installed product, validate a real PE image, freeze this candidate, or establish static GUI readiness. The tree review detects ordinary changes during enumeration and hashing; it does not claim a guarantee against arbitrary concurrent adversarial filesystem swaps. Even an accepted inventory report covers only its reported file/path/static-import checks; the inventory proposal keeps release qualification incomplete pending separate linker-map review.

To complete the v43 static chain, this candidate still needs a fresh source freeze, corrected OFF build receipt and link-map PASS, clean OFF install, schema-v2 report and external SHA256, installed GUI/Python/provider hashes, fresh authority and output pins, package/bootstrap receipts, all pending static gates, independent review, and a separate root decision. See `README-v43-static-chain.md` for the producer contract. Product GUI, Save, Run, and Confirm remain false in the preparation manifest.
