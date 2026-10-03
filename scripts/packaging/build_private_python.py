"""Build the locked Windows runtime offline from verified archives; never install pip.

Development Python runs this assembler. The resulting interpreter ignores user
site-packages, registry and PYTHONPATH, and ships its own NumPy/SciPy wheels.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile
import urllib.request
import zipfile


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def extract(archive, target):
    with zipfile.ZipFile(archive) as bundle:
        for entry in bundle.infolist():
            path = PurePosixPath(entry.filename)
            if (path.is_absolute() or ".." in path.parts or "\\" in entry.filename
                    or ":" in entry.filename or (entry.external_attr >> 16) & 0o170000 == 0o120000):
                raise ValueError("Archive contains an unsafe member")
            if entry.file_size > 256 * 1024 * 1024:
                raise ValueError("Archive member exceeds package limit")
            bundle.extract(entry, target)


def build(lock_path, cache, output, offline):
    if sys.platform != "win32":
        raise ValueError("This runtime assembler targets Windows AMD64")
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    if lock["schemaVersion"] != 1 or lock["platform"] != "win_amd64":
        raise ValueError("Unsupported runtime lock")
    if output.exists():
        raise ValueError("Output already exists; create a new immutable runtime directory")
    cache.mkdir(parents=True, exist_ok=True)
    for item in lock["files"]:
        filename = item["filename"]
        if Path(filename).name != filename:
            raise ValueError("Invalid cache filename")
        archive = cache / filename
        if not archive.exists():
            if offline:
                raise ValueError(f"Offline archive missing: {filename}")
            if not item["url"].startswith(("https://www.python.org/", "https://files.pythonhosted.org/")):
                raise ValueError("Unexpected dependency source")
            temporary = archive.with_suffix(archive.suffix + ".download")
            urllib.request.urlretrieve(item["url"], temporary)
            if sha(temporary) != item["sha256"]:
                temporary.unlink()
                raise ValueError(f"Downloaded archive hash mismatch: {filename}")
            temporary.replace(archive)
        if sha(archive) != item["sha256"]:
            raise ValueError(f"Cached archive hash mismatch: {filename}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="runtime-assembly-", dir=output.parent) as temporary:
        stage = Path(temporary) / "python"
        stage.mkdir()
        for item in lock["files"]:
            target = stage if item["name"] == "cpython" else stage / "Lib" / "site-packages"
            target.mkdir(parents=True, exist_ok=True)
            extract(cache / item["filename"], target)
        (stage / "python313._pth").write_text(
            "python313.zip\n.\nLib/site-packages\n../../share/range-workflow\n", encoding="utf-8")
        (stage / "runtime-lock.json").write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
        verification = subprocess.run([str(stage / "python.exe"), "-I", "-c",
            "import sys,numpy,scipy; import scipy.linalg; "
            "assert sys.flags.isolated and sys.flags.no_user_site; "
            "assert abs(float(scipy.linalg.det(numpy.array([[2.,0.],[0.,3.]])))-6.)<1e-12; "
            "print(sys.version.split()[0],numpy.__version__,scipy.__version__)"],
            capture_output=True, text=True, timeout=60, creationflags=0x08000000)
        if verification.returncode:
            raise ValueError("Private runtime verification failed: " + verification.stderr[-4000:])
        manifest = {str(p.relative_to(stage)).replace("\\", "/"): sha(p)
                    for p in sorted(stage.rglob("*")) if p.is_file() and "__pycache__" not in p.parts}
        (stage / "runtime-files.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        stage.replace(output)
    print(json.dumps({"runtimeId": lock["runtimeId"], "path": str(output.resolve()),
                      "verifiedVersions": verification.stdout.strip(), "offline": offline}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=Path(__file__).with_name("runtime-win-amd64.lock.json"))
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    build(args.lock, args.cache, args.output, args.offline)
