"""Test-only bridge to admission's live source re-enumerator."""

from __future__ import annotations

import json
from pathlib import Path
import sys

PREP_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PREP_ROOT))
sys.dont_write_bytecode = True

from admission_v43 import (  # noqa: E402
    SOURCE_SCOPE,
    SOURCE_SCOPE_SCHEMA,
    _enumerate_source_scope,
    _source_tree_digest,
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: admission-source-scope-probe-v43.py SOURCE_ROOT")
    rows, submodules, root_commit = _enumerate_source_scope(Path(sys.argv[1]))
    payload = {
        "sourceScopeSchema": SOURCE_SCOPE_SCHEMA,
        "sourceScope": SOURCE_SCOPE,
        "sourceFiles": rows,
        "sourceTreeSha256": _source_tree_digest(rows),
        "submoduleCount": len(submodules),
        "rootCommit": root_commit,
    }
    print(json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
