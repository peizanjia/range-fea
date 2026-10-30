# Source provenance and license inventory — 2026-10-30

This is a repository-level inventory for the current source handoff. It does not certify legal compliance, establish who authored each line, or make an academic originality claim. The portable phase status is in [the phase checkpoint](phase-checkpoint-20261030.md).

## Source scope

The root checkout is `peizanjia/range-fea`, branch `feature/structural-profile`, based on `9ea1d51545c52a0adc48c62bc1a65b68775c9ff3` before the phase root commit. The phase changes span the root application, solver, workflow, tests, documentation and CI, plus the following already-published submodule commits:

| Submodule | Branch | Published commit |
| --- | --- | --- |
| `src/range-ai-lib` | `codex/wf11-tool-diagnostics` | `66c79da` |
| `src/range-base-lib` | `codex/wf10-quiet-logs` | `25cca75` |
| `src/range-cloud-lib` | `codex/source-freeze-20261030` | `d34f2f5` |
| `src/range-gui-lib` | `codex/source-freeze-20261030` | `047c060` |
| `src/range-model-lib` | `feature/structural-profile` | `6a732d9` |
| `src/range-solver-lib` | `feature/structural-profile` | `880c5d1` |

The main source areas are trusted-mechanics/verification and experiment services; Agent authorization, diagnostics and evidence capture; axial, layered and solid piezo model/solver paths; external excitation/control bridges; GUI/workflow lifecycle; packaging and test infrastructure. The root commit records the integrated source and the six gitlinks; the submodule commits retain their own histories.

## License files found in this checkout

| Path | Text present | Handoff note |
| --- | --- | --- |
| `LICENSE` | GNU GPL version 3 | Root-level license file. |
| `src/range-base-lib/LICENSE`, `src/range-cloud-lib/LICENSE`, `src/range-gui-lib/LICENSE`, `src/range-model-lib/LICENSE`, `src/range-solver-lib/LICENSE`, `src/range-build-tools/LICENSE` | GNU GPL version 3 | License files present in those checked-out components. |
| `src/fea/doc/LICENSE.txt` | Range Software Package License, version 3, containing GPLv3 text and an acknowledgement section | Treat this as a distinct project notice; this inventory does not interpret its scope for every file or binary. |
| `src/TetGen/LICENSE` | TetGen dual-licensing notice: AGPLv3-or-later or a commercial license | Preserve the notice and check the selected distribution path and bundled artifacts before release. |
| `src/range-ai-lib` | No standalone `LICENSE` file was found at the checked-out submodule root | The absence of a file is not a license conclusion; ownership and applicable license need confirmation. |

This is a scan of license files at the repository and component roots, not a complete third-party or transitive dependency audit. It does not resolve the terms for every Qt module, Python wheel, OpenSSL build, packaged binary, or other vendored dependency. The checked-in dependency-provenance documents and the final package inventory should be reconciled before commercial or formal course distribution.

## Contribution and AI-assistance boundary

Git author fields identify the configured committer identity; they do not prove who designed, wrote, reviewed, or owns the changes. The phase includes AI-assisted planning, implementation, and review. No file-by-file human/AI attribution, upstream-origin comparison, or independent originality determination is asserted here. The user should review and confirm any personal-contribution statement before it is used in a course submission, publication, or external claim.

## Still required for a full delivery review

- Confirm the user's personal contribution description and the extent of AI assistance to disclose for the intended course rules.
- Review third-party and transitive dependency notices against the exact release/package contents.
- Resolve the standalone license/ownership metadata for `range-ai-lib` and confirm the scope of the Range Software Package notice.
- Keep this inventory separate from the course-stage acceptance result; passing a build or publishing source does not close the installed-product or live-Agent gates.
