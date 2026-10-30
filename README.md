# Range Software - FEA

## Current project and documentation

The current course-project workstream is the [Trusted Computation Agent, Stages 0–8](docs/TRUSTED_COMPUTATION_AGENT_PLAN.md): rod/beam modeling, diagnosis, independent verification, bounded numerical repair, convergence, Agent orchestration, controlled comparison, and course delivery. It builds on the existing Range FEA platform; the historical product stages and the separate PC, WF, and CW plans are not additional course stages.

Start with the [documentation index](docs/README.md) to find plans, delivery drafts, interaction records, and historical references. Use the [continuation guide](docs/CONTINUATION.md) for handoff and the portable [2026-10-30 phase checkpoint](docs/acceptance/phase-checkpoint-20261030.md) for current source and course-stage status. Raw `.planning/` receipts are local-only and are not included in a fresh clone; the checkpoint states which claims remain open. Navigation summaries do not replace source/build-bound evidence.

## Platform and historical product workstreams

The [development plan](docs/DEVELOPMENT_PLAN.md), [workflow delivery plan](docs/WORKFLOW_DELIVERY_PLAN.md), [machine-readable backlog](docs/workflow_delivery_plan.json), and [handoff](HANDOFF.md) distinguish accepted capabilities from remaining work. The [integrated workflow scope](docs/WORKFLOW_INTEGRATED_SCOPE.md) describes current-model snapshots, recovery, supervised task plans, accepted-step runtime parameters, series queries, trusted scientific extensions, and bounded geometry/mesh commands. The 20k-series performance gate, actual-provider scientific generation, and verified LPAC isolation remain open; ordinary generated execution is disabled and WF-13 release acceptance remains pending. Numerical scope remains defined by the [Stage 7](TECHNICAL_ROUTE_STAGE7.md), [Stage 8](TECHNICAL_ROUTE_STAGE8.md), and [Stage 9](TECHNICAL_ROUTE_STAGE9.md) routes; MATLAB software TCP validation is documented in [controller adapters](docs/CONTROLLER_ADAPTERS.md).

The [Stage 9 workflow](docs/STAGE9_CONTROL_ANALYSIS.md) connects native FE working-point matrices to Python descriptor export, modal/Craig–Bampton reduction, frequency response, control diagnostics, and offline collocated velocity feedback with full-order FE validation. Numerical scripts require NumPy and SciPy; the verified private-runtime installation includes these packages. Generated extension v1 retains its narrower contract; see the [extension SDK](docs/EXTENSION_SDK.md).

The [project catalog](docs/PROJECT_CATALOG.md) describes persistent document identities, native source hashes and immutable snapshots, the GUI Project menu, the `range-project` CLI, and read-only SDK queries. Task acceptance remains tracked in the workflow backlog.

For current workstream ownership, cross-platform continuation steps, and links to the authoritative stage/evidence ledgers, see the [documentation index](docs/README.md) and [continuation index](docs/CONTINUATION.md).

Project status, structural finite-element algorithms and meshing implementation are documented in [Stage 0-3 Technical Route](TECHNICAL_ROUTE_STAGE0_3.md). Structural constraints, couplings, sensors and actuators are documented in [Stage 4 Technical Route](TECHNICAL_ROUTE_STAGE4.md). The dynamics and control implementation is documented in [Stage 5 Technical Route](TECHNICAL_ROUTE_STAGE5.md) and [Stage 6 Technical Route](TECHNICAL_ROUTE_STAGE6.md). See [Agent Playbooks](docs/agent_playbooks/README.md) for interface guidance and the [development continuation guide](docs/agent_playbooks/workflow_delivery.md) to select, implement and verify the next WF task. From the repository root, run `python scripts/ci/verify_workflow_plan.py --next --json` for a read-only recommendation.

Range FEA is an open-source desktop application for computer-aided engineering using the Finite Element Method. It provides an integrated environment for mesh generation, simulation setup, and 3D visualisation of results across a range of physics domains — including structural stress, heat transfer, fluid dynamics, acoustics, electrostatics, and magnetostatics.

The application is built on Qt6 and ships with a standalone CLI solver (`fea-solver`) alongside the main GUI (`fea`). Mesh generation uses the embedded TetGen library for automatic tetrahedral meshing.

![Range Software](http://www.range-software.com/files/common/Range3-CFD.png)

## Help and tutorials

1. [General help](./src/fea/help/general/en/general_index.md)
2. [Tutorials](./src/fea/help/tutorials/en/tutorial_index.md)

## Prepare build environment
Initialize all submodules
```
git submodule update --init --recursive
```
The following command will attempt to download and install all required packages, therefore it must be executed under a privileged (root) user
```
sudo ./src/range-build-tools/prereqs.sh
```
_NOTE: In case your OS does not provide you with Qt version 6.8 or newer download and install it from [https://www.qt.io/download/](https://www.qt.io/download/)._
## Build
```
cmake -S src -B build-Release && \
cmake --build build-Release --parallel
```
## Create installation packages and installers
```
cmake --build build-Release --target package
```
## Installation using generated installers
### Windows

### MacOS
#### Drag & Drop
```
open './build-Release/range-fea-1.2.1-darwin-arm64.dmg'
```
### Linux
#### Self extracting Tar GZip
```
./build-Release/range-fea-1.2.1-linux-x86_64.sh
```
#### Debian DEB package
```
sudo apt install ./build-Release/range-fea-1.2.1-linux-x86_64.deb
```
#### RedHat RPM package
```
sudo dnf install ./build-Release/range-fea-1.2.1-linux-x86_64.rpm
```

## Download
To download already built binaries please visit http://range-software.com

## Powered by

* Qt - https://www.qt.io/
* TetGen - http://tetgen.org
