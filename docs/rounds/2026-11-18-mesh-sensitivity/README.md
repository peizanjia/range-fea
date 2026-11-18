# Results mesh sensitivity workflow

The Results panel exposes one **Mesh sensitivity…** action for the selected model. The workflow was planned around reuse of the current displacement field, one bounded solver launch per request, and a user-owned acceptance decision. The Assistant uses its existing read-only question panel.

## Workflow

1. Open the study for one linear static planar model. The window displays the fixed displacement monitoring points and Ux/Uy in millimetres. With no displacement monitors, the maximum-displacement node in the baseline is chosen and its coordinates are held fixed. Up to twelve distinct positions are supported.
2. Pass through the baseline gate. A native solve attached to the current document revision can be reused immediately. A loaded snapshot can also be reused when its native completion journal pins the result hash and the current geometry, analysis and displacement match. When the source state cannot be confirmed, **Solve baseline** runs that mesh once in isolation. Opening the window or asking the Assistant launches no solver.
3. Choose **Accept current** to explicitly record an engineering review of the baseline, or request one refinement. For TRI3, select a point in the table to refine locally. The options reveal its radius and the whole-mesh alternative. QUAD4 offers whole-mesh refinement. The solver process runs asynchronously and can be cancelled.
4. Compare the newest two valid levels at the same coordinates. The maximum absolute change across Ux and Uy is checked against the requested millimetre tolerance. The default 0.01 mm is an editable initial value. A user can adjust the local region or request another level. A comparison exceeding the tolerance disables acceptance. One passing adjacent comparison describes the measured displacement sensitivity for that refinement.
5. **Ask Assistant** supplies structured evidence, the refinement option, and the remaining budget. The existing sidecar can explain the evidence and recommend accepting, changing the model in the ordinary editor, or refining again. Applying those decisions remains in the normal UI. Editing the live source invalidates its study. Reopening the study on the same unchanged selected document in the current session reuses the measured levels.
6. **Accept current** records the decision, tolerance, result hash and measured levels in a timestamped JSON receipt. **Open accepted model** opens that native result separately. The source document remains available during refined solves and after acceptance.

The receipt distinguishes engineering review without a comparison from acceptance with a displacement comparison. It also records failed, cancelled, interrupted and time-limited attempts. Previous valid levels are available when a new attempt fails. A study can launch at most three attempts, with at most 200,000 elements and 120 seconds per solve. The optional postprocessor does not start successive jobs on its own.

## Supported refinement

The initial implementation is for constant-Z, linear elastic plane stress/strain TRI3 or QUAD4, with supported constant Cartesian displacement constraints, point or zero-area edge forces, and gravity. Surface groups retain their material, thickness and formulation. Existing nodes remain at their original coordinates. Triangle local refinement marks cells in the selected radius and those incident at the point. Any adjacent cell with a marked edge shares the new midpoint. If a loaded edge group is touched, the group splits consistently and each child receives half its parent segment force. Rectangular or convex quadrilateral cells split into four in the whole model.

The isolated solver input is pinned independently of the native result link. The result must match the candidate mesh, material, active constraints, loads and gravity within floating-point scale/unscale round-off. The workflow keeps the exact input geometry and settings for the next pass. Measurements use barycentric TRI3 and inverse bilinear QUAD4 interpolation, so the study can read a QUAD4 probe independently of the legacy native monitoring-point locator.

The study currently evaluates displacement. A local pass establishes sensitivity only to the chosen refined region. Arbitrary independent mesh comparisons, resuming a saved study after an application restart, stress error estimation, and Agent-triggered refinement are outside this version. Durable receipts remain available alongside the candidate snapshots, solver logs and result files under the project's .range-runs directory.

## Validation

Qt offscreen tests drive the actual review dialog and native solver process. The strip tests use analytical axial-force and force-plus-gravity oracles. They cover explicit starts, direct acceptance without additional solves, fixed-coordinate interpolation, local conformity, force preservation, Agent evidence handoff without a provider, stale and changed-load baseline rejection, baseline recomputation, cancellation, failed-attempt isolation and an exhausted attempt budget. The five-material coursework is also exercised through the dialog on TRI3 and QUAD4. Both reject the first comparison at 0.01 mm and accept after the next pass at the same tolerance.

| Coursework baseline | First pass maximum change | Second pass maximum change |
| --- | ---: | ---: |
| TRI3, local around P1, second radius halved | 0.02985 mm | 0.00529 mm |
| QUAD4, whole model | 0.03691 mm | 0.00639 mm |

Build targets are `fea`, `fea-solver` and `tst_mesh_sensitivity_dialog`. Run `ctest -R '^tst_mesh_sensitivity_dialog$'`. To include a coursework case, set `RANGE_MESH_COURSEWORK_RESULT` to its native record with its completion journal alongside it. Windows, a live configured provider, and installation packaging were outside the Linux verification. Local Qt 6.10, GCC 15 and OpenSSL build compatibility patches for pre-existing baseline files were applied only to the isolated build checkout. They are excluded from the feature commit.
