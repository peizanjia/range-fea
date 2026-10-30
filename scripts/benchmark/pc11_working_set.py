"""Read-only Windows PC-11 working-set diagnostic sampler.

Run manually with the installed GUI PID, exact executable, allowed descendant
image directories, an absolute fresh output directory and an absolute stop file.
This process-tree metric is informational; physical RAM acceptance is measured
separately. No process is launched, terminated, or modified by this helper.
"""
import argparse
import csv
import ctypes
from ctypes import wintypes
import json
import math
import os
from pathlib import Path
import time


EXIT_WAIT_BUDGET_MS = 5


class InvalidMeasurement(RuntimeError):
    pass


def checked_bytes(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0 or int(value) != value:
        raise InvalidMeasurement("invalid working-set query result")
    return int(value)


def canonical(path):
    return os.path.normcase(os.path.realpath(path))


class WindowsProcesses:
    def __init__(self):
        if os.name != "nt":
            raise InvalidMeasurement("Windows is required")
        class Entry(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                        ("pid", wintypes.DWORD), ("heap", ctypes.c_size_t),
                        ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                        ("parent", wintypes.DWORD), ("priority", wintypes.LONG),
                        ("flags", wintypes.DWORD), ("name", wintypes.WCHAR * 260)]
        class Memory(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in ("peakWorking", "working", "peakPaged", "paged",
                "peakNonpaged", "nonpaged", "pagefile", "peakPagefile")]
        self.Entry, self.Memory = Entry, Memory
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
        signatures = {
            "CreateToolhelp32Snapshot": ([wintypes.DWORD, wintypes.DWORD], wintypes.HANDLE),
            "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
            "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            "Process32FirstW": ([wintypes.HANDLE, ctypes.POINTER(Entry)], wintypes.BOOL),
            "Process32NextW": ([wintypes.HANDLE, ctypes.POINTER(Entry)], wintypes.BOOL),
            "GetProcessTimes": ([wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4, wintypes.BOOL),
            "QueryFullProcessImageNameW": ([wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                            ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = args, result
        self.psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]
        self.psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

    def fail(self, operation):
        raise OSError(ctypes.get_last_error(), operation)

    def enumerate(self):
        snapshot = self.kernel.CreateToolhelp32Snapshot(2, 0)
        if snapshot == wintypes.HANDLE(-1).value:
            self.fail("CreateToolhelp32Snapshot")
        entry = self.Entry(); entry.size = ctypes.sizeof(entry)
        result = {}
        try:
            valid = self.kernel.Process32FirstW(snapshot, ctypes.byref(entry))
            while valid:
                result[int(entry.pid)] = int(entry.parent)
                valid = self.kernel.Process32NextW(snapshot, ctypes.byref(entry))
            if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                self.fail("process enumeration incomplete")
            return result
        finally:
            self.close(snapshot)

    def open(self, pid):
        handle = self.kernel.OpenProcess(0x1000 | 0x10 | 0x100000, False, pid)
        if not handle:
            self.fail("OpenProcess pid=" + str(pid))
        return handle

    def close(self, handle):
        self.kernel.CloseHandle(handle)

    def times(self, handle):
        values = [wintypes.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(handle, *[ctypes.byref(v) for v in values]):
            self.fail("GetProcessTimes")
        return tuple((int(v.dwHighDateTime) << 32) | int(v.dwLowDateTime) for v in values[:2])

    def identity(self, handle):
        created, exited = self.times(handle)
        image = ctypes.create_unicode_buffer(32768); size = wintypes.DWORD(len(image))
        if not self.kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(size)):
            self.fail("QueryFullProcessImageNameW")
        return created, canonical(image.value)

    def exited(self, handle):
        return self.wait_exited(handle, 0)

    def wait_exited(self, handle, timeout_ms):
        # GetProcessTimes exit fields alone are not a live-state contract.
        # SYNCHRONIZE handles let the kernel prove this exact process ended.
        status = self.kernel.WaitForSingleObject(handle, timeout_ms)
        if status == 0:
            return True
        if status == 258:  # WAIT_TIMEOUT: exact pinned process is still alive.
            return False
        self.fail("WaitForSingleObject process exit state")

    def working_set(self, handle):
        memory = self.Memory(); memory.cb = ctypes.sizeof(memory)
        if not self.psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
            self.fail("GetProcessMemoryInfo")
        return checked_bytes(int(memory.working))


class Sampler:
    def __init__(self, api, root_pid, expected_image, allowed_roots, event):
        self.api, self.root_pid, self.event = api, root_pid, event
        self.expected = canonical(expected_image)
        self.allowed = [canonical(p) for p in allowed_roots]
        self.owned = {}
        self.exit_wait_deadline = None

    def identity_failure(self, pid, stage, error):
        self.event({"event": "identity-query-failed", "pid": pid, "stage": stage,
                    "error": str(error), "winerror": getattr(error, "winerror", None),
                    "errno": error.errno})

    def permitted(self, image):
        for root in self.allowed:
            try:
                if os.path.commonpath([root, image]) == root:
                    return True
            except ValueError:
                pass
        return False

    def confirmed_exit(self, pid, record, handle):
        """Accept only an exited handle matching this pinned process lifetime."""
        created, _ = self.api.times(handle)
        if created != record.get("creationTime100ns"):
            raise InvalidMeasurement("descendant PID reused during identity query")
        if not self.api.exited(handle):
            # Only a previously verified child can use the shared sample budget.
            # This is a kernel wait, never a cached identity or a live-process skip.
            if pid == self.root_pid or not record.get("image"):
                return False
            if self.exit_wait_deadline is None:
                self.exit_wait_deadline = time.monotonic() + EXIT_WAIT_BUDGET_MS / 1000
            remaining = max(0, int((self.exit_wait_deadline - time.monotonic()) * 1000))
            if not remaining:
                return False
            if not self.api.wait_exited(handle, remaining):
                return False
        # Exit-time contents read before the signal are undefined. Reread after.
        created, exited = self.api.times(handle)
        if created != record.get("creationTime100ns"):
            raise InvalidMeasurement("descendant PID reused during identity query")
        if not exited:
            raise InvalidMeasurement("signaled child lacks an exit timestamp")
        pinned_created, pinned_exit = self.api.times(record["handle"])
        if pinned_created != created or not self.api.exited(record["handle"]) or pinned_exit != exited:
            raise InvalidMeasurement("exited child does not match pinned process lifetime")
        if pid == self.root_pid:
            raise InvalidMeasurement("root exited during identity query")
        if record.get("observedExitTime100ns") != pinned_exit:
            record["observedExitTime100ns"] = pinned_exit
            self.event({"event": "verified-child-exited-during-identity-query", "pid": pid,
                        "creationTime100ns": created, "exitTime100ns": pinned_exit,
                        "image": record.get("image")})
        return True

    def adopt(self, pid, parent, snapshot):
        try:
            handle = self.api.open(pid)
        except OSError as error:
            if pid != self.root_pid and pid not in self.api.enumerate():
                if pid in snapshot.values():
                    raise InvalidMeasurement("exited unverified ancestor has descendants") from error
                self.event({"event": "child-exited-before-open", "pid": pid, "parentPid": parent})
                return
            raise InvalidMeasurement("alive/unreadable process pid=" + str(pid)) from error
        # Retain every opened owned handle until finalization, including failures.
        record = {"pid": pid, "parentPid": parent, "handle": handle}
        self.owned[pid] = record
        try:
            created, image = self.api.identity(handle)
        except OSError as error:
            self.identity_failure(pid, "adopt", error)
            if pid != self.root_pid and self.api.exited(handle):
                created, exited = self.api.times(handle)
                ancestor = self.owned[parent]
                if not exited or created < ancestor.get("creationTime100ns", created + 1) or self.api.exited(ancestor["handle"]):
                    raise InvalidMeasurement("exited unverified child lacks ancestor/lifetime proof") from error
                if pid in snapshot.values() or pid in self.api.enumerate().values():
                    raise InvalidMeasurement("exited unverified ancestor has descendants") from error
                record.update(creationTime100ns=created, observedExitTime100ns=exited, exitedBeforeIdentity=True)
                self.event({"event": "child-exited-before-identity", "pid": pid,
                            "parentPid": parent, "creationTime100ns": created, "exitTime100ns": exited})
                return
            raise InvalidMeasurement("unreadable process identity pid=" + str(pid)) from error
        record.update(creationTime100ns=created, image=image)
        if pid == self.root_pid:
            if image != self.expected:
                raise InvalidMeasurement("root executable mismatch")
        else:
            ancestor = self.owned[parent]
            if self.api.exited(ancestor["handle"]):
                raise InvalidMeasurement("cannot establish new descendant through exited ancestor")
            if "creationTime100ns" not in ancestor or created < ancestor["creationTime100ns"] or snapshot.get(pid) != parent:
                raise InvalidMeasurement("descendant ancestry/creation mismatch")
            record["parentCreationTime100ns"] = ancestor["creationTime100ns"]
            if not self.permitted(image):
                self.event({"event": "unknown-descendant", "pid": pid, "image": image})
                raise InvalidMeasurement("unknown descendant image: " + image)
        self.event({"event": "owned", **{k: v for k, v in record.items() if k != "handle"}})

    def sample(self):
        self.exit_wait_deadline = None
        snapshot = self.api.enumerate()
        if self.root_pid not in self.owned:
            if self.root_pid not in snapshot:
                raise InvalidMeasurement("root is absent")
            self.adopt(self.root_pid, snapshot[self.root_pid], snapshot)
        root = self.owned[self.root_pid]
        # A new handle validates the current PID against the pinned identity.
        check = self.api.open(self.root_pid)
        try:
            if self.api.identity(check) != (root["creationTime100ns"], root["image"]):
                raise InvalidMeasurement("root PID reused or executable changed")
        finally:
            self.api.close(check)
        if self.api.exited(root["handle"]):
            raise InvalidMeasurement("root exited before stop")
        for pid, record in self.owned.items():
            if pid == self.root_pid or pid not in snapshot:
                continue
            try:
                current = self.api.open(pid)
            except OSError as error:
                if self.confirmed_exit(pid, record, record["handle"]) and pid not in self.api.enumerate():
                    continue
                raise InvalidMeasurement("alive/unreadable descendant handle pid=" + str(pid)) from error
            try:
                try:
                    identity = self.api.identity(current)
                except OSError as error:
                    self.identity_failure(pid, "fresh", error)
                    if self.confirmed_exit(pid, record, current):
                        continue
                    raise InvalidMeasurement("alive/unreadable descendant identity pid=" + str(pid)) from error
                if identity != (record.get("creationTime100ns"), record.get("image")):
                    raise InvalidMeasurement("descendant PID reused or executable changed")
            finally:
                self.api.close(current)
        # Discover grandchildren in the same snapshot; retained parents preserve
        # ancestry even if a parent exited between sampling rounds.
        pending = dict(snapshot)
        while True:
            candidates = [(pid, parent) for pid, parent in pending.items()
                          if pid not in self.owned and parent in self.owned]
            if not candidates:
                break
            for pid, parent in candidates:
                pending.pop(pid)
                self.adopt(pid, parent, snapshot)
        total, processes = 0, []
        for pid, record in self.owned.items():
            if self.api.exited(record["handle"]):
                if pid == self.root_pid:
                    raise InvalidMeasurement("root exited during sample")
                continue
            if "creationTime100ns" not in record:
                raise InvalidMeasurement("live child has no verified identity")
            try:
                identity = self.api.identity(record["handle"])
            except OSError as error:
                self.identity_failure(pid, "pinned", error)
                if self.confirmed_exit(pid, record, record["handle"]):
                    continue
                raise InvalidMeasurement("alive/unreadable pinned identity pid=" + str(pid)) from error
            if identity != (record["creationTime100ns"], record["image"]):
                raise InvalidMeasurement("owned process identity changed")
            try:
                value = checked_bytes(self.api.working_set(record["handle"]))
            except OSError as error:
                if self.api.exited(record["handle"]):
                    if pid == self.root_pid:
                        raise InvalidMeasurement("root exited during query") from error
                    self.event({"event": "child-exited-during-query", "pid": pid})
                    continue
                raise InvalidMeasurement("alive/unreadable working set pid=" + str(pid)) from error
            total += value
            processes.append({"pid": pid, "workingSetBytes": value})
        return total, processes

    def proof(self):
        return [{k: v for k, v in record.items() if k != "handle"} for record in self.owned.values()]

    def close(self):
        for record in self.owned.values():
            self.api.close(record["handle"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root-pid", type=int, required=True)
    parser.add_argument("--expected-root-image", type=Path, required=True)
    parser.add_argument("--allowed-root", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stop-file", type=Path, required=True)
    parser.add_argument("--interval-ms", type=float, default=50)
    parser.add_argument("--min-samples", type=int, default=10)
    parser.add_argument("--min-seconds", type=float, default=0.5)
    args = parser.parse_args(argv)
    paths = [args.output, args.stop_file, args.expected_root_image, *args.allowed_root]
    if any(not p.is_absolute() for p in paths) or args.root_pid <= 0:
        parser.error("absolute paths and a positive root PID are required")
    if args.interval_ms != 50 or args.min_samples < 10 or not math.isfinite(args.min_seconds) or args.min_seconds < .5:
        parser.error("PC-11 requires 50 ms, at least 10 samples and at least 0.5 seconds")
    args.output.mkdir(parents=True, exist_ok=True)
    # Exclusive creation prevents accidental mixing with another run's evidence.
    start = time.monotonic_ns(); samples = []; events = []; sampler = None
    def event(value):
        value = {"monotonicNs": time.monotonic_ns(), **value}
        events.append(value)
        log.write(json.dumps(value, allow_nan=False) + "\n"); log.flush()
    summary = {"schema": "range.pc11-working-set/v1", "status": "UNMEASURED",
               "samplingIntervalMilliseconds": 50,
               "includeBaseline": True, "subtractSharedLibraries": False,
               "rootPid": args.root_pid, "expectedRootImage": canonical(args.expected_root_image),
               "startedMonotonicNs": start, "guiAcceptanceVerified": False}
    with (args.output / "ownership-events.jsonl").open("x", encoding="utf-8") as log, \
         (args.output / "working-set.csv").open("x", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["monotonic_ns", "elapsed_seconds", "working_set_bytes", "process_count", "processes_json", "sample_duration_ms"])
        try:
            sampler = Sampler(WindowsProcesses(), args.root_pid, args.expected_root_image, args.allowed_root, event)
            deadline = time.monotonic()
            while not args.stop_file.exists():
                stamp = time.monotonic_ns()
                total, processes = sampler.sample()
                duration = (time.monotonic_ns() - stamp) / 1e6
                samples.append((stamp, total))
                writer.writerow([stamp, (stamp - start) / 1e9, total, len(processes), json.dumps(processes), duration])
                stream.flush()
                deadline += .05
                time.sleep(max(0, deadline - time.monotonic()))
            elapsed = (time.monotonic_ns() - start) / 1e9
            summary["status"] = "MEASURED" if len(samples) >= args.min_samples and elapsed >= args.min_seconds else "UNMEASURED"
            event({"event": "stop-file-observed", "status": summary["status"]})
        except (OSError, InvalidMeasurement) as error:
            summary.update(status="INVALID", error=str(error), errorType=type(error).__name__)
            event({"event": "invalid", "error": str(error)})
        finally:
            summary.update(endedMonotonicNs=time.monotonic_ns(), sampleCount=len(samples),
                           maximumWorkingSetBytes=max((s[1] for s in samples), default=None),
                           firstSampleMonotonicNs=samples[0][0] if samples else None,
                           lastSampleMonotonicNs=samples[-1][0] if samples else None,
                           maximumSampleGapMilliseconds=max(((b[0]-a[0])/1e6 for a,b in zip(samples,samples[1:])), default=None))
            proof = sampler.proof() if sampler else []
            if sampler:
                sampler.close()
            (args.output / "ownership-proof.json").write_text(json.dumps(proof, indent=2, allow_nan=False), encoding="utf-8")
            (args.output / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False), encoding="utf-8")
    print(json.dumps(summary, allow_nan=False))
    return 0 if summary["status"] == "MEASURED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
