"""Read-only terminal admission for an actual GUI-owned native job.

Validates immutable registry blob/marker SHA chains and the final checkpoint
envelope. Does not decode/certify trace records or create a numeric provider.
The subsequent real GUI request must independently certify all 20001 records.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import importlib.util

# The installed embedded Python intentionally excludes arbitrary script dirs
# from sys.path. Load this explicit audit helper by its actual local path.
_spec = importlib.util.spec_from_file_location("pc11_audit_verify", Path(__file__).with_name("pc11_gui_verify.py"))
verify = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(verify)


MARKER = re.compile(r"^commit-r([0-9]{8})-([0-9a-f]{32})-([0-9a-f]{64})-([0-9a-f]{64})\.marker$")
SELF_HASH = re.compile(rb',"nativeJobRegistrySha256":"([0-9a-f]{64})"')


def regular(path, maximum):
    verify.require(path.is_file() and not path.is_symlink() and path.stat().st_size <= maximum, "Missing/linked/oversized authority: " + str(path))
    verify.require(verify.same_path(path, path.resolve()), "Noncanonical authority")
    return path.read_bytes()


def registry(directory):
    entries = list(directory.iterdir())
    verify.require(len(entries) <= 512, "Registry directory bound exceeded")
    legacy = directory / "job.json"
    markers = sorted((int(m.group(1)), path, m) for path in entries if (m := MARKER.match(path.name)))
    if not markers and not legacy.exists():
        return None
    verify.require(len(markers) <= 128, "Registry marker bound exceeded")
    expected, previous, latest, proof = 1, "0" * 64, None, []
    if legacy.exists():
        raw = regular(legacy, 32 * 1024 * 1024)
        latest = json.loads(raw)
        hashes = SELF_HASH.findall(raw)
        verify.require(len(hashes) == 1 and hashes[0].decode() == latest["nativeJobRegistrySha256"], "Invalid native snapshot self-hash field")
        without_hash = SELF_HASH.sub(b"", raw, count=1)
        verify.require(hashlib.sha256(without_hash).hexdigest() == latest["nativeJobRegistrySha256"], "Native snapshot self-hash mismatch")
        reduced = dict(latest); reduced.pop("nativeJobRegistrySha256")
        verify.require(json.loads(without_hash) == reduced, "Native snapshot hash removal altered unrelated fields")
        verify.require(latest["nativeJobRegistrySchema"] == "range.native-job-record/v1" and int(latest["nativeJobRegistryRevision"]) >= 1, "Native snapshot schema/revision mismatch")
        verify.require(latest["jobId"] == directory.name and latest["projectId"] == directory.parent.name and verify.same_path(latest["nativeJobRecordRef"], legacy), "Native snapshot identity/path mismatch")
        verify.require(legacy.read_bytes() == raw, "Native atomic snapshot changed during observation")
        expected = latest["nativeJobRegistryRevision"] + 1
        previous = latest["nativeJobRegistrySha256"]
        proof.append({"storage": "atomic-native-job-snapshot", "blob": str(legacy), "blobSha256": hashlib.sha256(raw).hexdigest(), "registrySha256": previous, "revision": latest["nativeJobRegistryRevision"]})
    for revision, path, match in markers:
        verify.require(revision == expected and match.group(3) == previous, "Broken registry revision/SHA chain")
        verify.require(regular(path, 0) == b"", "Nonempty commit marker")
        blob = directory / ("job-record-r%08d-%s.json" % (revision, match.group(2)))
        raw = regular(blob, 32 * 1024 * 1024)
        blob_sha = hashlib.sha256(raw).hexdigest()
        verify.require(blob_sha == match.group(4), "Registry marker/blob hash mismatch")
        latest = json.loads(raw)
        hashes = SELF_HASH.findall(raw)
        verify.require(len(hashes) == 1 and hashes[0].decode() == latest["nativeJobRegistrySha256"], "Invalid registry self-hash field")
        without_hash = SELF_HASH.sub(b"", raw, count=1)
        verify.require(hashlib.sha256(without_hash).hexdigest() == latest["nativeJobRegistrySha256"], "Registry self-hash mismatch")
        reduced = dict(latest); reduced.pop("nativeJobRegistrySha256")
        verify.require(json.loads(without_hash) == reduced, "Registry self-hash byte removal altered unrelated fields")
        verify.require(latest["nativeJobRegistrySchema"] == "range.native-job-record/v1" and latest["nativeJobRegistryRevision"] == revision, "Registry schema/revision mismatch")
        verify.require(latest["nativeJobRegistryPreviousSha256"] == previous, "Registry previous hash mismatch")
        verify.require(latest["jobId"] == directory.name and latest["projectId"] == directory.parent.name, "Registry job/project directory mismatch")
        verify.require(verify.same_path(latest["nativeJobRecordRef"], blob), "Registry record path mismatch")
        previous = latest["nativeJobRegistrySha256"]; expected += 1
        proof.append({"marker": str(path), "blob": str(blob), "blobSha256": blob_sha, "registrySha256": previous})
    return latest, proof


def terminal(profile, gui_pid):
    root = profile / "application-data" / "local-data" / "native-jobs"
    if not root.exists():
        return {"status": "WAITING", "reason": "GUI native registry not yet created"}
    found = []
    for project in root.iterdir():
        if not project.is_dir():
            continue
        for job in project.iterdir():
            if not job.is_dir():
                continue
            read = registry(job)
            if read and int(read[0].get("ownerPid", -1)) == gui_pid:
                found.append(read)
    verify.require(len(found) <= 1, "Multiple GUI-owned jobs; explicit identity selection required")
    if not found:
        return {"status": "WAITING", "reason": "GUI-owned committed job absent"}
    record, chain = found[0]
    if record["state"] in ("failed", "cancelled", "interrupted"):
        return {"status": "FAILED_JOB", "jobId": record["jobId"], "state": record["state"], "record": record}
    if record["state"] != "succeeded":
        return {"status": "WAITING", "jobId": record["jobId"], "state": record["state"]}
    for flag in ("resultArtifactVerified", "resultSourceBindingVerified", "resultReadbackComplete", "nativeResultCatalogAvailable"):
        verify.require(record[flag] is True, "Terminal GUI job lacks product verification: " + flag)
    directory = Path(record["nativeCheckpointDirectory"])
    verify.require(not (directory / "pending-publication.json").exists(), "Terminal native publication remains PENDING")
    pointer_raw = regular(directory / "latest.json", 65536)
    pointer = json.loads(pointer_raw)
    verify.require(pointer["schema"] == "range.native-checkpoint-pointer/v2", "Complete trace checkpoint pointer required")
    name = pointer["envelopeFile"]
    verify.require(Path(name).name == name and "/" not in name and "\\" not in name and ":" not in name, "Envelope basename escaped checkpoint root")
    envelope_path = directory / name
    envelope_raw = regular(envelope_path, 1024 * 1024)
    verify.require(hashlib.sha256(envelope_raw).hexdigest() == pointer["envelopeSha256"], "Terminal checkpoint envelope SHA mismatch")
    envelope = json.loads(envelope_raw)
    verify.require(envelope["schema"] == "range.native-checkpoint/v2" and envelope["acceptedSeq"] == 20000, "Native job did not complete 20000 accepted FE steps")
    context = record["nativeCheckpointContext"]
    verify.require(envelope["inputModelSha256"] == context["modelSha256"], "Envelope original input binding mismatch")
    for key in ("sourceSha256", "controlSha256", "runtimeSha256", "packageSha256", "dtS", "policy", "maxAcceptedSteps", "maxSimulationSeconds"):
        verify.require(envelope[key] == context[key], "Envelope context mismatch: " + key)
    result = Path(record["resultArtifactRef"])
    verify.require(verify.sha(result) == record["resultArtifactSha256"], "Final product result artifact SHA mismatch")
    verify.require(result.parent.name == "run-results" and result.parent.parent == directory, "Native result escaped checkpoint run-results root")
    verify.require((directory / envelope["traceFile"]).is_file(), "Final accepted trace missing")
    verify.require((directory / "latest.json").read_bytes() == pointer_raw, "Final checkpoint pointer changed during terminal observation")
    return {"status": "TERMINAL_NATIVE_20000", "jobId": record["jobId"], "projectId": record["projectId"], "guiPid": gui_pid,
            "recordRef": record["nativeJobRecordRef"], "registryChain": chain, "resultModel": str(result), "resultArtifactSha256": record["resultArtifactSha256"],
            "acceptedSeq": envelope["acceptedSeq"], "envelopeFile": str(envelope_path), "envelopeSha256": pointer["envelopeSha256"],
            "stateFingerprintSha256": envelope["stateFingerprintSha256"], "traceRecordCertificationPerformed": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--gui-pid", type=int, required=True)
    args = parser.parse_args()
    try:
        result = terminal(args.profile, args.gui_pid)
    except (OSError, ValueError, KeyError, TypeError) as error:
        result = {"status": "INVALID", "error": str(error)}
    print(json.dumps(result, allow_nan=False))
    return 0 if result["status"] == "TERMINAL_NATIVE_20000" else 1 if result["status"] == "WAITING" else 2


if __name__ == "__main__":
    sys.exit(main())
