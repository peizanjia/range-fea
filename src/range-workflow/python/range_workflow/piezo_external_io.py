"""Bounded, synchronous range-external-io/1 transport and pure-state SDK.

The caller owns accepted state and FE transactions. A successful evaluate only
returns a candidate; this module never commits it. Local file hashes establish
artifact identity, not purity or a sandbox. TCP identity remains self-reported.
"""
from __future__ import annotations

import copy
import errno
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import queue
import select
import shutil
import socket
import subprocess
import threading
import time

PROTOCOL = "range-external-io/1"
MAX_LINE = 1024 * 1024
ROLES = ("excitation", "controller")
UNITS = {"force": "N", "moment": "N m", "displacement": "m",
         "velocity": "m/s", "acceleration": "m/s^2", "rotation": "rad",
         "angularVelocity": "rad/s", "angularAcceleration": "rad/s^2", "voltage": "V", "charge": "C"}


class ExternalIOError(ValueError):
    """Invalid configuration, stale identity, cancellation or protocol failure."""


def _validate_json(value, depth=0):
    if depth > 32:
        raise ExternalIOError("JSON nesting exceeds 32")
    if value is None or isinstance(value, (str, bool)):
        return
    if type(value) in (int, float):
        try:
            finite = math.isfinite(value)
        except OverflowError:
            finite = False
        if not finite:
            raise ExternalIOError("non-finite JSON number")
        return
    if isinstance(value, dict) and all(isinstance(k, str) for k in value):
        for item in value.values():
            _validate_json(item, depth + 1)
        return
    if isinstance(value, list):
        for item in value:
            _validate_json(item, depth + 1)
        return
    raise ExternalIOError("unsupported JSON value")


def encode(value):
    _validate_json(value)
    try:
        return json.dumps(value, ensure_ascii=False, allow_nan=False,
                          separators=(",", ":"), sort_keys=True).encode("utf-8")
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ExternalIOError("invalid JSON encoding") from exc


def decode(line):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ExternalIOError("duplicate JSON field")
            result[key] = value
        return result
    try:
        if line.startswith(b"\xef\xbb\xbf"):
            raise ExternalIOError("UTF-8 BOM is forbidden")
        result = json.loads(line.decode("utf-8"), object_pairs_hook=unique,
                            parse_constant=lambda x: (_ for _ in ()).throw(ExternalIOError("non-finite JSON number")))
        _validate_json(result)
        if not isinstance(result, dict):
            raise ExternalIOError("JSON object required")
        return result
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ExternalIOError(str(exc)) from exc


def state_digest(state):
    return hashlib.sha256(encode(state)).hexdigest()


def _text(value, label):
    if not isinstance(value, str) or not value.strip() or len(value) > 4096 or "\0" in value:
        raise ExternalIOError(f"{label} must be a nonempty bounded string")
    return value


def _number(value, label):
    if type(value) not in (int, float):
        raise ExternalIOError(f"{label} must be a finite number")
    try:
        result = float(value)
    except OverflowError as exc:
        raise ExternalIOError(f"{label} must be finite") from exc
    if not math.isfinite(result):
        raise ExternalIOError(f"{label} must be finite")
    return result


def _keys(value, required, optional=()):
    if not isinstance(value, dict) or not set(required) <= set(value) or set(value) - set(required) - set(optional):
        raise ExternalIOError("missing or unknown fields: " + ", ".join(required))


def _check_budget(deadline=None, cancel_event=None):
    if cancel_event is not None and cancel_event.is_set():
        raise ExternalIOError("external I/O cancelled")
    if deadline is not None and time.monotonic() >= deadline:
        raise ExternalIOError("external I/O deadline exceeded")


def normalize_config(config, *, deadline=None, cancel_event=None):
    """Validate connection settings without launching or connecting."""
    _check_budget(deadline, cancel_event)
    _keys(config, ("transport", "algorithm"), ("executable", "args", "cwd", "dependencies",
          "host", "port", "handshakeTimeout", "requestTimeout", "maxLineBytes", "expectedIdentity"))
    result = copy.deepcopy(config)
    _keys(result["algorithm"], ("id", "version"))
    for key in ("id", "version"):
        _text(result["algorithm"][key], "algorithm " + key)
    for key, default in (("handshakeTimeout", 30.), ("requestTimeout", 5.)):
        result[key] = _number(result.get(key, default), key)
        if not 0 < result[key] <= 55:
            raise ExternalIOError(key + " must be in (0,55] seconds (host budget)")
    result.setdefault("maxLineBytes", MAX_LINE)
    if type(result["maxLineBytes"]) is not int or not 256 <= result["maxLineBytes"] <= 16 * MAX_LINE:
        raise ExternalIOError("maxLineBytes must be 256..16777216")
    if result["transport"] == "stdio":
        if "host" in result or "port" in result:
            raise ExternalIOError("TCP fields on stdio configuration")
        executable = _text(result.get("executable"), "executable")
        resolved = shutil.which(executable)
        if resolved is None and Path(executable).is_file():
            resolved = executable
        if resolved is None:
            raise ExternalIOError("executable not found")
        result["executable"] = str(Path(resolved).resolve())
        result["cwd"] = str(Path(result.get("cwd", Path.cwd())).resolve())
        if not Path(result["cwd"]).is_dir():
            raise ExternalIOError("working directory does not exist")
        result.setdefault("args", [])
        if not isinstance(result["args"], list) or any(not isinstance(a, str) or "\0" in a for a in result["args"]):
            raise ExternalIOError("args must be a separate string list")
        dependencies = result.get("dependencies", [])
        if not isinstance(dependencies, list) or any(not isinstance(p, str) for p in dependencies):
            raise ExternalIOError("dependencies must be file paths")
        paths = []
        for name in dependencies:
            path = Path(name)
            path = path if path.is_absolute() else Path(result["cwd"]) / path
            if not path.is_file():
                raise ExternalIOError("dependency file unavailable: " + str(path))
            paths.append(str(path.resolve()))
        # Scripts passed as arguments are identities even if omitted by the UI.
        for arg in result["args"]:
            path = Path(arg)
            path = path if path.is_absolute() else Path(result["cwd"]) / path
            if path.is_file():
                paths.append(str(path.resolve()))
        result["dependencies"] = sorted(set(paths))
    elif result["transport"] == "tcp":
        if any(k in result for k in ("executable", "args", "cwd", "dependencies")):
            raise ExternalIOError("process fields on TCP configuration")
        host = result.get("host", "127.0.0.1")
        if not isinstance(host, str):
            raise ExternalIOError("TCP host must be a string")
        try:
            local = host == "localhost" or ipaddress.ip_address(host).is_loopback
        except ValueError:
            local = False
        if not local:
            raise ExternalIOError("TCP host must be a literal loopback address or localhost")
        result["host"] = "127.0.0.1" if host == "localhost" else host
        if type(result.get("port")) is not int or not 1 <= result["port"] <= 65535:
            raise ExternalIOError("TCP port must be 1..65535")
    else:
        raise ExternalIOError("transport must be stdio or tcp")
    if "expectedIdentity" in result:
        _validate_json(result["expectedIdentity"])
        if result["expectedIdentity"] != _connection_identity(result, deadline=deadline, cancel_event=cancel_event):
            raise ExternalIOError("external executable/dependency identity changed; reload required")
    return result


def _connection_identity(config, *, deadline=None, cancel_event=None):
    files = []
    if config["transport"] == "stdio":
        for name in sorted(set([config["executable"]] + config["dependencies"])):
            _check_budget(deadline, cancel_event)
            digest = hashlib.sha256()
            try:
                with open(name, "rb") as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        _check_budget(deadline, cancel_event)
                        digest.update(block)
            except OSError as exc:
                raise ExternalIOError("identity file unavailable: " + name) from exc
            files.append({"path": name, "sha256": digest.hexdigest()})
    return {"transport": config["transport"], "identityTrust": "local-files" if files else "self-reported",
            "algorithm": copy.deepcopy(config["algorithm"]), "files": files}


def connection_identity(config, *, deadline=None, cancel_event=None):
    return _connection_identity(normalize_config(config, deadline=deadline, cancel_event=cancel_event),
                                deadline=deadline, cancel_event=cancel_event)


inspect_config = connection_identity


def fingerprint(config, *, deadline=None, cancel_event=None):
    normalized = normalize_config(config, deadline=deadline, cancel_event=cancel_event)
    return state_digest({"config": normalized,
                         "identity": _connection_identity(normalized, deadline=deadline, cancel_event=cancel_event)})


def validate_ports(ports):
    if not isinstance(ports, list) or not ports:
        raise ExternalIOError("nonempty port list required")
    result, ids = [], set()
    for port in ports:
        _keys(port, ("id", "direction", "role", "quantity", "unit", "reference"),
              ("coordinateSystem", "positiveDirection", "initialValue", "binding"))
        p = copy.deepcopy(port)
        _text(p["id"], "port id")
        if p["id"] in ids:
            raise ExternalIOError("duplicate port id")
        ids.add(p["id"])
        if p["role"] not in ROLES or p["direction"] not in ("input", "output"):
            raise ExternalIOError("invalid port role or direction")
        if not isinstance(p["quantity"], str) or p["quantity"] not in UNITS or p["unit"] != UNITS[p["quantity"]]:
            raise ExternalIOError("port quantity/unit mismatch")
        if p["reference"] not in ("absolute", "increment"):
            raise ExternalIOError("invalid port reference")
        if p["direction"] == "output":
            if p["role"] == "controller" and (p["quantity"] != "voltage" or p["reference"] != "absolute"):
                raise ExternalIOError("controller output requires absolute voltage")
            if p["role"] == "excitation" and p["quantity"] not in ("force", "moment"):
                raise ExternalIOError("excitation output requires force/moment")
        if p["role"] == "excitation" and p["direction"] == "input":
            raise ExternalIOError("excitation may not read FE inputs")
        p.setdefault("coordinateSystem", "global")
        p.setdefault("positiveDirection", "+")
        p.setdefault("initialValue", 0.)
        p.setdefault("binding", {})
        _text(p["coordinateSystem"], "coordinateSystem")
        _text(p["positiveDirection"], "positiveDirection")
        p["initialValue"] = _number(p["initialValue"], "initialValue")
        if not isinstance(p["binding"], dict):
            raise ExternalIOError("port binding must be an object")
        _validate_json(p)
        result.append(p)
    return result


def _values(values, ports):
    if not isinstance(values, list):
        raise ExternalIOError("signal values must be a list")
    expected = {p["id"]: p for p in ports}
    seen = set()
    for value in values:
        _keys(value, ("id", "value", "unit", "reference"))
        name = value["id"]
        if not isinstance(name, str) or name in seen or name not in expected:
            raise ExternalIOError("duplicate or unknown signal port")
        seen.add(name)
        port = expected[name]
        if value["unit"] != port["unit"] or value["reference"] != port["reference"]:
            raise ExternalIOError("signal unit/reference mismatch")
        _number(value["value"], "signal value")
    if seen != set(expected):
        raise ExternalIOError("missing signal port")


class _WindowsOwnedProcess:
    """Create a provider *inside* a kill-on-close job, without an assignment race.

    PROC_THREAD_ATTRIBUTE_JOB_LIST binds membership atomically in CreateProcessW
    (Windows 10+). The private job handle is never inherited. Thus an abrupt
    backend exit closes its last handle and terminates the entire provider tree.
    Failing job setup fails launch; there is no uncontained fallback.
    https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute
    """
    def __init__(self, args, *, cwd):
        import ctypes as ct
        from ctypes import wintypes as wt
        import msvcrt

        self.args = args
        self.returncode = None
        self._handle = self._job = None
        self.stdin = self.stdout = self.stderr = None
        self._ct = ct
        self._kernel = kernel = ct.WinDLL("kernel32", use_last_error=True)

        class BasicLimit(ct.Structure):
            _fields_ = [("processTime", ct.c_longlong), ("jobTime", ct.c_longlong),
                ("flags", wt.DWORD), ("minimumWorkingSet", ct.c_size_t),
                ("maximumWorkingSet", ct.c_size_t), ("activeProcesses", wt.DWORD),
                ("affinity", ct.c_size_t), ("priority", wt.DWORD), ("scheduling", wt.DWORD)]

        class ExtendedLimit(ct.Structure):
            _fields_ = [("basic", BasicLimit), ("ioCounters", ct.c_ulonglong * 6),
                ("processMemory", ct.c_size_t), ("jobMemory", ct.c_size_t),
                ("peakProcessMemory", ct.c_size_t), ("peakJobMemory", ct.c_size_t)]

        class StartupInfo(ct.Structure):
            _fields_ = [("cb", wt.DWORD), ("reserved", wt.LPWSTR),
                ("desktop", wt.LPWSTR), ("title", wt.LPWSTR),
                ("x", wt.DWORD), ("y", wt.DWORD), ("width", wt.DWORD), ("height", wt.DWORD),
                ("xChars", wt.DWORD), ("yChars", wt.DWORD), ("fill", wt.DWORD),
                ("flags", wt.DWORD), ("showWindow", wt.WORD), ("reservedSize", wt.WORD),
                ("reservedBytes", ct.POINTER(ct.c_ubyte)),
                ("stdin", wt.HANDLE), ("stdout", wt.HANDLE), ("stderr", wt.HANDLE)]

        class StartupInfoEx(ct.Structure):
            _fields_ = [("startup", StartupInfo), ("attributes", ct.c_void_p)]

        class ProcessInfo(ct.Structure):
            _fields_ = [("process", wt.HANDLE), ("thread", wt.HANDLE),
                        ("pid", wt.DWORD), ("tid", wt.DWORD)]

        signatures = {
            "CreateJobObjectW": ([ct.c_void_p, wt.LPCWSTR], wt.HANDLE),
            "SetInformationJobObject": ([wt.HANDLE, ct.c_int, ct.c_void_p, wt.DWORD], wt.BOOL),
            "CloseHandle": ([wt.HANDLE], wt.BOOL),
            "InitializeProcThreadAttributeList": ([ct.c_void_p, wt.DWORD, wt.DWORD, ct.POINTER(ct.c_size_t)], wt.BOOL),
            "UpdateProcThreadAttribute": ([ct.c_void_p, wt.DWORD, ct.c_size_t, ct.c_void_p,
                                          ct.c_size_t, ct.c_void_p, ct.c_void_p], wt.BOOL),
            "DeleteProcThreadAttributeList": ([ct.c_void_p], None),
            "CreateProcessW": ([wt.LPCWSTR, wt.LPWSTR, ct.c_void_p, ct.c_void_p, wt.BOOL,
                                wt.DWORD, ct.c_void_p, wt.LPCWSTR, ct.c_void_p,
                                ct.POINTER(ProcessInfo)], wt.BOOL),
            "WaitForSingleObject": ([wt.HANDLE, wt.DWORD], wt.DWORD),
            "GetExitCodeProcess": ([wt.HANDLE, ct.POINTER(wt.DWORD)], wt.BOOL),
            "TerminateProcess": ([wt.HANDLE, wt.UINT], wt.BOOL),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(kernel, name)
            function.argtypes, function.restype = arguments, result
        fds = []
        attributes = None
        try:
            self._job = kernel.CreateJobObjectW(None, None)
            if not self._job:
                raise ct.WinError(ct.get_last_error())
            limits = ExtendedLimit()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not kernel.SetInformationJobObject(self._job, 9, ct.byref(limits), ct.sizeof(limits)):
                raise ct.WinError(ct.get_last_error())
            size = ct.c_size_t()
            kernel.InitializeProcThreadAttributeList(None, 2, 0, ct.byref(size))
            if not size.value:
                raise ct.WinError(ct.get_last_error())
            storage = ct.create_string_buffer(size.value)
            if not kernel.InitializeProcThreadAttributeList(storage, 2, 0, ct.byref(size)):
                raise ct.WinError(ct.get_last_error())
            attributes = storage
            jobs = (wt.HANDLE * 1)(self._job)
            if not kernel.UpdateProcThreadAttribute(attributes, 0, 0x2000D,
                    jobs, ct.sizeof(jobs), None, None):  # PROC_THREAD_ATTRIBUTE_JOB_LIST
                raise ct.WinError(ct.get_last_error())
            stdin_read, stdin_write = os.pipe()
            fds.extend((stdin_read, stdin_write))
            stdout_read, stdout_write = os.pipe()
            fds.extend((stdout_read, stdout_write))
            stderr_read, stderr_write = os.pipe()
            fds.extend((stderr_read, stderr_write))
            child_handles = [msvcrt.get_osfhandle(fd) for fd in (stdin_read, stdout_write, stderr_write)]
            for handle in child_handles:
                os.set_handle_inheritable(handle, True)
            inherited = (wt.HANDLE * 3)(*child_handles)
            if not kernel.UpdateProcThreadAttribute(attributes, 0, 0x20002,
                    inherited, ct.sizeof(inherited), None, None):  # PROC_THREAD_ATTRIBUTE_HANDLE_LIST
                raise ct.WinError(ct.get_last_error())
            startup = StartupInfoEx()
            startup.startup.cb = ct.sizeof(startup)
            startup.startup.flags = 0x101  # STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW
            startup.startup.showWindow = 0
            startup.startup.stdin, startup.startup.stdout, startup.startup.stderr = child_handles
            startup.attributes = ct.cast(attributes, ct.c_void_p)
            process = ProcessInfo()
            command = ct.create_unicode_buffer(subprocess.list2cmdline(args))
            if not kernel.CreateProcessW(args[0], command, None, None, True,
                    0x08080000, None, cwd, ct.byref(startup), ct.byref(process)):
                # CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT
                raise ct.WinError(ct.get_last_error())
            self._handle, self.pid = process.process, process.pid
            kernel.CloseHandle(process.thread)
            self.stdin = os.fdopen(stdin_write, "wb", buffering=0)
            fds.remove(stdin_write)
            self.stdout = os.fdopen(stdout_read, "rb", buffering=0)
            fds.remove(stdout_read)
            self.stderr = os.fdopen(stderr_read, "rb", buffering=0)
            fds.remove(stderr_read)
        except BaseException:
            self.close_job()
            if self._handle:
                kernel.WaitForSingleObject(self._handle, 600)
                kernel.CloseHandle(self._handle)
                self._handle = None
            for stream in (self.stdin, self.stdout, self.stderr):
                if stream:
                    stream.close()
            raise
        finally:
            if attributes is not None:
                kernel.DeleteProcThreadAttributeList(attributes)
            for fd in fds:
                os.close(fd)

    def poll(self):
        if self.returncode is None and self._kernel.WaitForSingleObject(self._handle, 0) == 0:
            from ctypes import wintypes
            code = wintypes.DWORD()
            if not self._kernel.GetExitCodeProcess(self._handle, self._ct.byref(code)):
                raise self._ct.WinError(self._ct.get_last_error())
            self.returncode = code.value
        return self.returncode

    def wait(self, timeout=None):
        milliseconds = 0xFFFFFFFF if timeout is None else min(0xFFFFFFFE, max(0, math.ceil(timeout * 1000)))
        result = self._kernel.WaitForSingleObject(self._handle, milliseconds)
        if result == 258:
            raise subprocess.TimeoutExpired(self.args, timeout)
        if result != 0:
            raise self._ct.WinError(self._ct.get_last_error())
        return self.poll()

    def terminate(self):
        if self.poll() is None and not self._kernel.TerminateProcess(self._handle, 1):
            if self.poll() is None:
                raise self._ct.WinError(self._ct.get_last_error())

    kill = terminate

    def close_job(self):
        if self._job:
            self._kernel.CloseHandle(self._job)
            self._job = None

    def close_handles(self):
        self.close_job()
        if self._handle:
            self._kernel.CloseHandle(self._handle)
            self._handle = None

    def __del__(self):
        if getattr(self, "_kernel", None):
            self.close_handles()


class ExternalIOClient:
    def __init__(self, config, *, run_id, model_digest, config_digest, mapping_digest,
                 ports, seed=0, cancel_event=None):
        self.config = normalize_config(config)
        self.identity = connection_identity(self.config)
        self._fingerprint = fingerprint(self.config)
        self.ports = validate_ports(ports)
        self.run_id = _text(run_id, "runId")
        self.digests = {}
        for key, value in (("modelDigest", model_digest), ("configDigest", config_digest), ("mappingDigest", mapping_digest)):
            if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
                raise ExternalIOError(key + " must be a complete SHA-256")
            self.digests[key] = value
        _validate_json(seed)
        self.seed = seed
        self.cancel_event = cancel_event or threading.Event()
        self._closed = threading.Event()
        self._lock = threading.Lock()
        self._dispose_lock = threading.Lock()
        self._responses = queue.Queue(maxsize=2)
        self._failure = None
        self._process = self._socket = None
        self._ready = False
        self._serial = 0
        self.stderr_tail = bytearray()

    def _remaining(self, deadline):
        if self.cancel_event.is_set() or self._closed.is_set():
            raise ExternalIOError("external I/O cancelled or closed")
        if self._failure:
            raise ExternalIOError(self._failure)
        left = deadline - time.monotonic()
        if left <= 0:
            raise ExternalIOError("external I/O deadline exceeded")
        return left

    def _connect(self, deadline):
        if fingerprint(self.config, deadline=deadline, cancel_event=self.cancel_event) != self._fingerprint:
            raise ExternalIOError("external executable/dependency identity changed")
        if self.config["transport"] == "stdio":
            try:
                arguments = [self.config["executable"], *self.config["args"]]
                if os.name == "nt":
                    self._process = _WindowsOwnedProcess(arguments, cwd=self.config["cwd"])
                else:
                    self._process = subprocess.Popen(arguments, cwd=self.config["cwd"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, shell=False)
            except OSError as exc:
                raise ExternalIOError("cannot launch external process") from exc
            def drain():
                try:
                    while not self._closed.is_set():
                        chunk = self._process.stderr.read(4096)
                        if not chunk:
                            break
                        self.stderr_tail.extend(chunk)
                        del self.stderr_tail[:-8192]
                except (OSError, ValueError):
                    pass
            threading.Thread(target=drain, daemon=True).start()
        else:
            family = socket.AF_INET6 if ":" in self.config["host"] else socket.AF_INET
            self._socket = socket.socket(family, socket.SOCK_STREAM)
            self._socket.setblocking(False)
            try:
                code = self._socket.connect_ex((self.config["host"], self.config["port"]))
                if code not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY, 10035, 10036, 10037):
                    raise OSError(code, "localhost connect failed")
                while code:
                    _, writable, failed = select.select([], [self._socket], [self._socket], min(.02, self._remaining(deadline)))
                    if writable or failed:
                        code = self._socket.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                        if code:
                            raise OSError(code, "localhost connect failed")
                self._socket.setblocking(True)
            except OSError as exc:
                raise ExternalIOError("cannot connect localhost endpoint") from exc
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        pending = bytearray()
        try:
            while not self._closed.is_set():
                chunk = self._socket.recv(65536) if self._socket else self._process.stdout.read(65536)
                if not chunk:
                    raise ExternalIOError("external endpoint closed")
                pending.extend(chunk)
                while b"\n" in pending:
                    line, _, tail = pending.partition(b"\n")
                    if len(line) + 1 > self.config["maxLineBytes"]:
                        raise ExternalIOError("response line exceeds limit")
                    self._responses.put_nowait(bytes(line).removesuffix(b"\r"))
                    pending = bytearray(tail)
                if len(pending) >= self.config["maxLineBytes"]:
                    raise ExternalIOError("response line exceeds limit")
        except (OSError, ValueError, queue.Full) as exc:
            if not self._closed.is_set():
                self._failure = str(exc) or "unsolicited responses exceed buffer"

    def _exchange(self, op, fields, timeout, deadline=None):
        if not self._lock.acquire(blocking=False):
            raise ExternalIOError("only one external request may be in flight")
        try:
            deadline = min(deadline, time.monotonic() + timeout) if deadline is not None else time.monotonic() + timeout
            self._remaining(deadline)
            if op != "close" and fingerprint(self.config, deadline=deadline, cancel_event=self.cancel_event) != self._fingerprint:
                raise ExternalIOError("external executable/dependency identity changed")
            if not self._process and not self._socket:
                self._connect(deadline)
            if not self._responses.empty():
                raise ExternalIOError("unsolicited external response")
            self._serial += 1
            request = {"protocol": PROTOCOL, "op": op, "runId": self.run_id,
                       "requestId": str(self._serial), **fields}
            data = encode(request) + b"\n"
            if len(data) > self.config["maxLineBytes"]:
                raise ExternalIOError("request line exceeds limit")
            written = threading.Event()
            errors = []
            def write():
                try:
                    if self._socket:
                        self._socket.sendall(data)
                    else:
                        view = memoryview(data)
                        while view:
                            count = self._process.stdin.write(view)
                            if not count:
                                raise ExternalIOError("external write failed")
                            view = view[count:]
                        self._process.stdin.flush()
                except (OSError, ValueError) as exc:
                    errors.append(exc)
                finally:
                    written.set()
            threading.Thread(target=write, daemon=True).start()
            while not written.wait(min(.02, self._remaining(deadline))):
                pass
            if errors:
                raise ExternalIOError("external write failed") from errors[0]
            while True:
                try:
                    line = self._responses.get(timeout=min(.02, self._remaining(deadline)))
                    break
                except queue.Empty:
                    pass
            response = decode(line)
            if not self._responses.empty():
                raise ExternalIOError("unsolicited extra external response")
            for key in ("protocol", "op", "runId", "requestId"):
                if response.get(key) != request[key] or type(response.get(key)) is not str:
                    raise ExternalIOError("response envelope mismatch: " + key)
            return response
        except (ExternalIOError, OSError) as exc:
            self._dispose()
            if isinstance(exc, ExternalIOError):
                raise
            raise ExternalIOError(str(exc)) from exc
        finally:
            self._lock.release()

    def handshake(self, deadline=None):
        if self._ready:
            raise ExternalIOError("connection already handshaken")
        response = self._exchange("handshake", {**self.digests, "ports": self.ports,
            "seed": self.seed, "algorithm": self.config["algorithm"], "identity": self.identity},
            self.config["handshakeTimeout"], deadline)
        try:
            _keys(response, ("protocol", "op", "runId", "requestId", "algorithm", "ports", "capabilities", "initialState", *self.digests))
            if any(response[key] != value for key, value in self.digests.items()):
                raise ExternalIOError("handshake model/configuration/mapping digest mismatch")
            if response["algorithm"] != self.config["algorithm"]:
                raise ExternalIOError("algorithm identity mismatch")
            if validate_ports(response["ports"]) != self.ports:
                raise ExternalIOError("handshake port mapping mismatch")
            if not isinstance(response["capabilities"], dict) or response["capabilities"].get("explicitState") is not True:
                raise ExternalIOError("endpoint must declare explicitState capability")
            roles = {p["role"] for p in self.ports}
            if not isinstance(response["initialState"], dict) or set(response["initialState"]) != roles:
                raise ExternalIOError("initialState requires exactly the registered role namespaces")
            self._ready = True
            return copy.deepcopy(response)
        except ExternalIOError:
            self._dispose()
            raise

    def evaluate(self, role, *, step, tick, time, sample_period, state, inputs=None,
                 previous_applied=None, valid_until=None, deadline=None):
        if not self._ready:
            raise ExternalIOError("handshake required before evaluate")
        if role not in ROLES or not any(p["role"] == role for p in self.ports):
            raise ExternalIOError("unregistered role")
        for label, count in (("step", step), ("tick", tick)):
            if not isinstance(count, str) or not count.isascii() or not count.isdecimal():
                raise ExternalIOError(label + " must be a decimal counter string")
        inputs = copy.deepcopy(inputs if inputs is not None else [])
        previous_applied = copy.deepcopy(previous_applied if previous_applied is not None else [])
        role_ports = [p for p in self.ports if p["role"] == role]
        outputs = [p for p in role_ports if p["direction"] == "output"]
        _values(inputs, [p for p in role_ports if p["direction"] == "input"])
        _values(previous_applied, outputs if role == "controller" else [])
        when = _number(time, "time")
        period = _number(sample_period, "samplePeriod")
        until = _number(valid_until, "validUntil") if valid_until is not None else when + period
        if period <= 0 or not math.isfinite(until) or until <= when:
            raise ExternalIOError("invalid simulation sample interval")
        digest = state_digest(state)
        response = self._exchange("evaluate", {"role": role, "step": step, "tick": tick,
            "time": when, "samplePeriod": period, "validUntil": until,
            "acceptedStateDigest": digest, "state": copy.deepcopy(state),
            "inputs": inputs, "previousApplied": previous_applied}, self.config["requestTimeout"], deadline)
        try:
            _keys(response, ("protocol", "op", "runId", "requestId", "step", "tick", "acceptedStateDigest", "outputs", "candidateState"))
            for key, expected in (("step", step), ("tick", tick), ("acceptedStateDigest", digest)):
                if response[key] != expected:
                    raise ExternalIOError("response state/step mismatch: " + key)
            _values(response["outputs"], outputs)
            return copy.deepcopy(response)
        except ExternalIOError:
            self._dispose()
            raise

    def _dispose(self):
        self._closed.set()
        if not self._dispose_lock.acquire(blocking=False):
            return
        try:
            if self._socket:
                try:
                    self._socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self._socket.close()
            if self._process:
                if isinstance(self._process, _WindowsOwnedProcess):
                    self._process.close_job()
                    # Job closure already requests tree termination. A second
                    # TerminateProcess can report ACCESS_DENIED while that
                    # asynchronous termination is pending.
                    self._process.wait(timeout=.6)
                elif self._process.poll() is None:
                    self._process.terminate()
                    try:
                        self._process.wait(timeout=.3)
                    except subprocess.TimeoutExpired:
                        self._process.kill()
                        self._process.wait(timeout=.3)
                for stream in (self._process.stdin, self._process.stdout, self._process.stderr):
                    try:
                        stream.close()
                    except (OSError, ValueError):
                        pass
                if isinstance(self._process, _WindowsOwnedProcess):
                    self._process.close_handles()
        finally:
            self._dispose_lock.release()

    def close(self):
        if self._closed.is_set():
            return
        if self._ready and not self._lock.locked() and not self.cancel_event.is_set():
            try:
                self._exchange("close", {}, .2)
            except ExternalIOError:
                pass
        self._dispose()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def serve(reader, writer, *, algorithm, initialize, evaluate, close=None, max_line_bytes=MAX_LINE):
    """Minimal blocking SDK. No idle deadline: FE may calculate between requests.

    initialize(handshake) returns per-role initial state; evaluate(request) must
    return (outputs, candidateState) using only request state. Diagnostics belong
    on stderr. Callback state is copied; the SDK stores no accepted state.
    """
    bound = None
    last_id = None
    while True:
        line = reader.readline(max_line_bytes + 1)
        if not line:
            return
        if len(line) > max_line_bytes or not line.endswith(b"\n"):
            raise ExternalIOError("request line exceeds limit or is unterminated")
        request = decode(line)
        for field in ("runId", "requestId"):
            _text(request.get(field), field)
        if request.get("protocol") != PROTOCOL or request.get("op") not in ("handshake", "evaluate", "close"):
            raise ExternalIOError("unknown protocol or operation")
        request_id = request["requestId"]
        if not request_id.isascii() or not request_id.isdecimal() or (last_id is not None and int(request_id) <= int(last_id)):
            raise ExternalIOError("request ID must be an increasing decimal string")
        last_id = request["requestId"]
        response = {k: request[k] for k in ("protocol", "op", "runId", "requestId")}
        if request["op"] == "handshake":
            _keys(request, ("protocol", "op", "runId", "requestId", "algorithm", "ports", "seed", "identity",
                            "modelDigest", "configDigest", "mappingDigest"))
            if bound is not None or request.get("algorithm") != algorithm:
                raise ExternalIOError("handshake algorithm/sequence mismatch")
            ports = validate_ports(request.get("ports"))
            bound = {"runId": request["runId"], "ports": ports}
            for key in ("modelDigest", "configDigest", "mappingDigest"):
                value = request[key]
                if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
                    raise ExternalIOError("invalid handshake digest")
                response[key] = value
            initial = initialize(copy.deepcopy(request))
            if not isinstance(initial, dict) or set(initial) != {p["role"] for p in ports}:
                raise ExternalIOError("initialState requires registered role namespaces")
            response.update(algorithm=algorithm, ports=ports,
                capabilities={"explicitState": True, "futurePreview": False},
                initialState=initial)
        else:
            if bound is None or request["runId"] != bound["runId"]:
                raise ExternalIOError("handshake/run identity mismatch")
            if request["op"] == "close":
                _keys(request, ("protocol", "op", "runId", "requestId"))
                if close:
                    close()
            else:
                _keys(request, ("protocol", "op", "runId", "requestId", "role", "step", "tick", "time", "samplePeriod",
                               "validUntil", "acceptedStateDigest", "state", "inputs", "previousApplied"))
                if request["role"] not in ROLES or not any(p["role"] == request["role"] for p in ports):
                    raise ExternalIOError("unregistered role")
                for key in ("step", "tick"):
                    count = request[key]
                    if not isinstance(count, str) or not count.isascii() or not count.isdecimal():
                        raise ExternalIOError("invalid counter string")
                when = _number(request["time"], "time")
                period = _number(request["samplePeriod"], "samplePeriod")
                until = _number(request["validUntil"], "validUntil")
                if period <= 0 or until <= when:
                    raise ExternalIOError("invalid simulation sample interval")
                if state_digest(request.get("state")) != request.get("acceptedStateDigest"):
                    raise ExternalIOError("accepted state digest mismatch")
                role_ports = [p for p in bound["ports"] if p["role"] == request.get("role")]
                _values(request["inputs"], [p for p in role_ports if p["direction"] == "input"])
                _values(request["previousApplied"], [p for p in role_ports if p["direction"] == "output"] if request["role"] == "controller" else [])
                outputs, candidate = evaluate(copy.deepcopy(request))
                _values(outputs, [p for p in role_ports if p["direction"] == "output"])
                response.update({k: request[k] for k in ("step", "tick", "acceptedStateDigest")})
                response.update(outputs=outputs, candidateState=candidate)
        data = encode(response) + b"\n"
        if len(data) > max_line_bytes:
            raise ExternalIOError("response line exceeds limit")
        writer.write(data)
        writer.flush()
        if request["op"] == "close":
            return


def main(argv=None):
    """Offline identity inspection or handshake-only connection test for the UI."""
    import argparse
    import sys
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--inspect", metavar="CONNECTION_JSON")
    group.add_argument("--test", metavar="SPEC_JSON")
    args = parser.parse_args(argv)
    try:
        path = Path(args.inspect or args.test)
        if path.stat().st_size > MAX_LINE:
            raise ExternalIOError("configuration file exceeds limit")
        spec = decode(path.read_bytes())
        if args.inspect:
            result = {"ok": True, "connection": normalize_config(spec), "identity": inspect_config(spec), "fingerprint": fingerprint(spec)}
        else:
            _keys(spec, ("connection", "ports"), ("runId", "modelDigest", "configDigest", "mappingDigest", "seed"))
            with ExternalIOClient(spec["connection"], run_id=spec.get("runId", "connection-test"),
                    model_digest=spec.get("modelDigest", state_digest({})),
                    config_digest=spec.get("configDigest", fingerprint(spec["connection"])),
                    mapping_digest=spec.get("mappingDigest", state_digest(spec["ports"])),
                    ports=spec["ports"], seed=spec.get("seed", 0)) as client:
                result = {"ok": True, "handshake": client.handshake(), "identity": client.identity}
        sys.stdout.buffer.write(encode(result) + b"\n")
        return 0
    except (ExternalIOError, OSError) as exc:
        sys.stdout.buffer.write(encode({"ok": False, "error": str(exc)}) + b"\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
