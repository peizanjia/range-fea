"""Read-only exact owned registry/prefix proof for normal GUI cancellation."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import time

spec = importlib.util.spec_from_file_location("pc11_terminal", Path(__file__).with_name("pc11_gui_terminal.py"))
terminal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(terminal)


def observe(profile, pid, job_id):
    found = []
    root = profile / "application-data/local-data/native-jobs"
    for project in root.iterdir():
        for directory in project.iterdir():
            if not directory.is_dir():
                continue
            read = terminal.registry(directory)
            if read and int(read[0].get("ownerPid", -1)) == pid:
                found.append(read)
    terminal.verify.require(len(found) == 1 and found[0][0]["jobId"] == job_id, "Expected exactly one matching GUI-owned job")
    record, proof = found[0]
    checkpoint = Path(record["nativeCheckpointDirectory"])
    prefix = None
    for attempt in range(5):
        raw = terminal.regular(checkpoint / "latest.json", 65536)
        pointer = json.loads(raw)
        name = pointer["envelopeFile"]
        terminal.verify.require(Path(name).name == name and not any(c in name for c in "/\\:"), "Envelope escaped checkpoint directory")
        envelope_raw = terminal.regular(checkpoint / name, 1024 * 1024)
        terminal.verify.require(hashlib.sha256(envelope_raw).hexdigest() == pointer["envelopeSha256"], "Accepted-prefix envelope SHA mismatch")
        envelope = json.loads(envelope_raw)
        terminal.verify.require(0 <= envelope["acceptedSeq"] <= 20000, "Accepted prefix is out of bounds")
        context = record["nativeCheckpointContext"]
        terminal.verify.require(envelope["inputModelSha256"] == context["modelSha256"], "Accepted prefix input binding differs")
        for key in ("sourceSha256", "controlSha256", "runtimeSha256", "packageSha256", "dtS", "policy", "maxAcceptedSteps", "maxSimulationSeconds"):
            terminal.verify.require(envelope[key] == context[key], "Accepted prefix context differs: " + key)
        if (checkpoint / "latest.json").read_bytes() == raw:
            prefix = {"acceptedSeq": envelope["acceptedSeq"], "pointerSha256": hashlib.sha256(raw).hexdigest(),
                      "envelopeFile": str(checkpoint / name), "envelopeSha256": pointer["envelopeSha256"],
                      "stateFingerprintSha256": envelope["stateFingerprintSha256"], "traceRecordCertificationPerformed": False}
            break
        time.sleep(.01)
    terminal.verify.require(prefix is not None, "Accepted prefix changed throughout observation")
    return {"status": "OWNED_JOB_REGISTRY_OBSERVED", "guiPid": pid, "jobId": job_id,
            "record": record, "registryProof": proof, "acceptedPrefix": prefix, "qualification": "NONE"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--gui-pid", type=int, required=True)
    parser.add_argument("--job-id", required=True)
    args = parser.parse_args()
    try:
        result = observe(args.profile, args.gui_pid, args.job_id)
        code = 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        result, code = {"status": "INVALID", "error": str(error)}, 2
    print(json.dumps(result, allow_nan=False))
    raise SystemExit(code)
