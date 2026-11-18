# Adaptive TRI3 refinement for static elasticity

**Solver → Start solver → Adaptive local refinement** opts in to one bounded adaptive review. A plain static solve keeps its existing execution path. Enabling the option reveals one numeric setting, the relative energy indicator target, initially 5%. The study opens in the existing mesh sensitivity window and uses its isolated solver, source revision checks, result inspection and acceptance receipt.

## Numerical contract

The initial scope is small-strain, planar, linear elastic TRI3 plane stress or plane strain. All cells must be conforming. The estimator includes constant gravity and distributed exterior line forces. It requires the supported Cartesian displacement constraints and mechanical constitutive law. Nonzero point forces, thermal expansion, interior line forces, coupled/active setups and higher-order or quadrilateral elements are rejected by the adaptive path. The existing manual QUAD4 study is available separately.

For each triangle recover constant strain from nodal displacement and form the in-plane membrane resultant N = tσ. Use b = tρg as the area load and a = t min(2μ, 2(μ+λ)) as the positive stiffness weight. The contribution from the cell interior is h² A |b|²/a, where h is the longest cell edge. Constant TRI3 stress has zero interior divergence. An exterior edge of length L contributes L²|Nn−q|²/a, where q is the prescribed force per edge length. An internal edge contributes L²|N₁n₁+N₂n₂|² / max(a₁,a₂), shared equally by the two triangles. Components constrained along the entire edge or cell are omitted from their corresponding residuals. An isolated nodal support leaves the surrounding free-edge residuals intact.

Sum the contributions to obtain η². Use η/√(2U), where U is recovered strain energy, as the relative indicator. This is a stiffness-weighted residual indicator, with neither a calibrated error bound nor a local peak-stress guarantee. A zero residual qualifies for an immediate stop, including a fully restrained body-load problem. When stored stresses jump at a material interface, the estimator compares the normal traction resultants on the two sides.

Dörfler marking sorts triangles by decreasing contribution, using a stable tie break, and selects the shortest prefix carrying at least 50% of η². The subdivision uses shared midpoints and conforming transitions. A touched force group splits consistently so the specified line force and moment are preserved. The new mesh is assembled and solved globally in the native solver. The prior mesh and all completed results remain available.

## Runtime and acceptance

- A verified state is assessed before launching a solver. A baseline below the target needs zero extra solves. An unverified or missing baseline gets one isolated solve when the user invokes the mode.
- The stop reasons are target reached, no adequate improvement over two successive refinements, resource limit, source change, cancellation and failure. The hold point is after a verified solution, before launching the next candidate.
- The cumulative budget is three launches, 200,000 elements, 120 s per solve and 180 s per adaptive review. A baseline solve counts toward the three launches. The minimum new child edge length is one millionth of the baseline span from its first node. Resource checks account for conformity and loaded-line splits before submitting a candidate.
- The evidence records the indicator, energy, marked count, candidate element count and leading marked cell locations per level. The user can open the last valid result even after a budget stop. A new explicit review can revise the target and reuse the last valid mesh with the remaining launch budget. The last three review settings and outcomes stay in the receipt.
- The existing read-only Assistant can interpret the evidence. The user records acceptance. The adaptive receipt distinguishes acceptance by the residual estimate from acceptance in a fixed-point displacement comparison.

## Verification

Build targets were `fea`, `fea-solver` and `tst_mesh_sensitivity_dialog` on Linux. `ctest -R '^tst_mesh_sensitivity_dialog$'` drives the dialog and the actual native solver, including retained baseline, cancellation, physical force and gravity, interface traction, and regression of manual sensitivity review. Provide `RANGE_MESH_COURSEWORK_RESULT` for a local coursework result with its native completion journal alongside the model.

The separate L-shaped benchmark uses a clamped left edge and uniform downward force on the right edge. On each mesh the largest contribution was at the reentrant corner (1, 1) m. For an illustrative common target of 67%, two adaptive solves used 925 elements at the final level and reached 66.2%; one whole-mesh refinement used 2,460 elements and reached 59.1%. The summed solver times excluding the baseline were approximately 0.23 and 0.39 s on this workstation. Both counts include line elements. These timings describe the specific benchmark and not a general speedup guarantee.

The five-material coursework used the previously solved coarse TRI3 baseline. Three automatic local passes gave 33.39% → 27.23% → 21.64% → 16.78% and 2,526 → 2,889 → 4,225 → 7,295 total elements. The 16.69% test target was still just unmet when the launch budget ended; the final valid solve remained available. A separate single global half-size pass reached 19.67% with 9,926 elements. At the common comparison threshold of 20%, both methods produced a qualifying mesh. The local series used fewer final elements and more wall time (about 3.3 s for all three local passes and 2.1 s for the global pass, including dialog preparation and artifact handling).

Linux verification used the existing isolated Qt/GCC/OpenSSL compatibility overlay, kept outside the feature commit. The tests cover the production study widget and native solver process. The solver-start entry was compiled into the full application; a live GUI end-to-end click through that separate entry and external Assistant provider behavior were outside the test run.
