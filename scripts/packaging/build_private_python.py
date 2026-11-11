#!/usr/bin/env python3
"""Stable entry point used by the existing Windows CI workflow."""

from pathlib import Path
from runpy import run_path


if __name__ == "__main__":
    implementation = Path(__file__).with_name("python-runtime") / "build_private_python.py"
    run_path(str(implementation), run_name="__main__")
