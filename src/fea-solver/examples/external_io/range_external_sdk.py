"""Source-tree Python SDK entry point for range-external-io/1.

For an installed workflow runtime, import range_workflow.piezo_external_io
directly. This helper makes the adjacent examples runnable from any directory.
"""
from pathlib import Path
import sys

_workflow = Path(__file__).resolve().parents[3] / "range-workflow" / "python"
if _workflow.is_dir():
    sys.path.insert(0, str(_workflow))

_installed = Path(__file__).resolve().parents[3] / "share" / "range-workflow"
if _installed.is_dir():
    sys.path.insert(0, str(_installed))

from range_workflow.piezo_external_io import (  # noqa: E402,F401
    ExternalIOError, PROTOCOL, decode, encode, serve, state_digest,
)
