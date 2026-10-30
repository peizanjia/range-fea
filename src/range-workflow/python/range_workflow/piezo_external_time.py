"""Frozen trusted time-only Python sources for the native NDJSON service.

This is neither a sandbox nor a proof of purity/dependency completeness. The
caller times out hung trusted code. Standard output streams and CRT descriptors
are quarantined with bounded accounting; intentional process/Win32 tampering is
outside this trusted interface. Declared Python siblings load from frozen bytes,
without stale sys.modules or bytecode reuse. Non-Python dependencies are files.
"""
from __future__ import annotations

import builtins
from contextlib import contextmanager, redirect_stdout, redirect_stderr
import hashlib
import inspect
import marshal
import os
from pathlib import Path
import platform
import re
import sys
import threading
from types import ModuleType

import numpy as np
import scipy

from .piezo_excitation import ExcitationError, ExternalTimeFunction, _digest, _keys


_SOURCE_PATH = Path(__file__).resolve()
_SOURCE_BYTES = _SOURCE_PATH.read_bytes()
_SOURCE_HASH = hashlib.sha256(_SOURCE_BYTES).hexdigest()
_CODE_HASH = hashlib.sha256(marshal.dumps(sys._getframe().f_code)).hexdigest()
# A broken output transport cannot report a recoverable business rejection.
# The native caller treats this nonzero process exit as fatal, without retrying.
FATAL_OUTPUT_EXIT_CODE = 74


class _RejectOutput:
    def write(self, value):
        if value:
            raise ExcitationError("external function must not write stdout/stderr")
        return 0

    def flush(self):
        pass


class _WindowsStreams:
    """Common dynamic CRT tables and process handles, with explicit ownership."""

    def __init__(self):
        import ctypes
        import msvcrt
        self.ctypes, self.msvcrt = ctypes, msvcrt
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        for name, args, result in (
                ("GetCurrentProcess", [], ctypes.c_void_p),
                ("TerminateProcess", [ctypes.c_void_p, ctypes.c_uint], ctypes.c_int),
                ("GetModuleHandleW", [ctypes.c_wchar_p], ctypes.c_void_p),
                ("GetStdHandle", [ctypes.c_ulong], ctypes.c_void_p),
                ("SetStdHandle", [ctypes.c_ulong, ctypes.c_void_p], ctypes.c_int),
                ("DuplicateHandle", [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                    ctypes.POINTER(ctypes.c_void_p), ctypes.c_ulong, ctypes.c_int,
                    ctypes.c_ulong], ctypes.c_int),
                ("CloseHandle", [ctypes.c_void_p], ctypes.c_int)):
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = args, result
        self.crts, self.loaded_modules = [], set()
        # Load both before changing any descriptors: a lazily initialized CRT
        # would otherwise inherit the temporary drain handle as its baseline.
        for name in ("ucrtbase", "msvcrt"):
            ctypes.CDLL(name)
        self.refresh()

    def refresh(self):
        """Discover loaded dynamic CRTs; never load optional CRTs speculatively."""
        names = ("ucrtbase", "ucrtbased", "msvcrt", "msvcrtd")+tuple(
            "msvcr"+version+suffix for version in ("70", "71", "80", "90", "100", "110", "120")
            for suffix in ("", "d"))
        added = []
        for name in names:
            handle = self.kernel.GetModuleHandleW(name+".dll")
            if not handle or handle in self.loaded_modules:
                continue
            crt = self.ctypes.CDLL(name)
            ctypes = self.ctypes
            try:
                for symbol, args, result in (
                    ("fflush", [ctypes.c_void_p], ctypes.c_int),
                    ("_dup", [ctypes.c_int], ctypes.c_int),
                    ("_dup2", [ctypes.c_int, ctypes.c_int], ctypes.c_int),
                    ("_close", [ctypes.c_int], ctypes.c_int),
                    ("_get_osfhandle", [ctypes.c_int], ctypes.c_ssize_t),
                    ("_open_osfhandle", [ctypes.c_ssize_t, ctypes.c_int], ctypes.c_int)):
                    function = getattr(crt, symbol)
                    function.argtypes, function.restype = args, result
            except AttributeError as exc:
                raise ExcitationError("unsupported loaded Windows CRT: "+name) from exc
            self.crts.append(crt)
            self.loaded_modules.add(handle)
            added.append(crt)
        return added

    def duplicate_handle(self, handle):
        duplicate = self.ctypes.c_void_p()
        process = self.kernel.GetCurrentProcess()
        if not self.kernel.DuplicateHandle(process, handle, process,
                self.ctypes.byref(duplicate), 0, False, 2):
            raise ExcitationError("external stdout/stderr drain handle duplication failed")
        return duplicate.value

    def descriptor(self, crt, handle):
        duplicate = self.duplicate_handle(handle)
        # _open_osfhandle owns a successfully opened handle, including on cleanup.
        fd = crt._open_osfhandle(duplicate, os.O_WRONLY | os.O_BINARY)
        if fd < 0:
            if not self.kernel.CloseHandle(duplicate):
                self.fatal_restore()
            raise ExcitationError("external stdout/stderr drain descriptor open failed")
        return fd

    def adopt_loaded(self, saved, handles):
        """Prepare complete new CRT pairs before publishing any backup targets."""
        self.refresh()
        adopted = {id(crt) for crt, _, _ in saved}
        pending = [crt for crt in self.crts if id(crt) not in adopted]
        if not pending:
            return
        if len(handles) != 2:
            raise ExcitationError("external stdout/stderr adoption has no complete process baseline")
        prepared = []
        try:
            for crt in pending:
                for fd, (_, original, owner, _) in zip((1, 2), handles):
                    if owner:
                        backup = next(backup for old, old_fd, backup in saved
                            if old is owner[0] and old_fd == owner[1])
                        original = owner[0]._get_osfhandle(backup)
                    prepared.append((crt, fd, self.descriptor(crt, original)))
        except BaseException:
            # Targets still point to the quarantined Win32 handles. Roll back
            # only newly owned backups; leave both target descriptors untouched.
            for crt, _, backup in reversed(prepared):
                if crt._close(backup) != 0:
                    self.fatal_restore()
            raise
        saved.extend(prepared)

    def flush(self):
        # Include native FILE buffers, not just Python's stream wrappers. fflush
        # runs before switching and while the drain is still installed on exit.
        for crt in self.crts:
            crt.fflush(None)

    def redirect(self, write_fd, saved, handles):
        originals = {}
        for crt in self.crts:
            for fd in (1, 2):
                handle = crt._get_osfhandle(fd)
                backup = crt._dup(fd)
                if backup < 0:
                    raise ExcitationError("external stdout/stderr descriptor backup failed")
                saved.append((crt, fd, backup))
                originals.setdefault(handle, (crt, fd))
        # Snapshot all tables first: inherited handles may be shared between
        # CRTs, and _dup2 can close a handle still named by another CRT/table.
        for number in (-11, -12):
            original = self.kernel.GetStdHandle(number)
            handles.append([number, original, originals.get(original), None])
        pipe_handle = self.msvcrt.get_osfhandle(write_fd)
        pipes = []
        try:
            for crt in self.crts:
                pipes.append((crt, self.descriptor(crt, pipe_handle)))
            for entry in handles:
                entry[3] = self.duplicate_handle(pipe_handle)
            # Detach every inherited alias before allocating replacement handles.
            # Otherwise _dup2 may reuse a just-closed numeric handle that another
            # CRT still names and subsequently closes. A repeated CloseHandle on
            # an inherited alias may fail; _close still releases its CRT slot.
            for crt in self.crts:
                for fd in (1, 2):
                    crt._close(fd)
            for crt, pipe_fd in pipes:
                for fd in (1, 2):
                    if crt._dup2(pipe_fd, fd) != 0:
                        raise ExcitationError("external stdout/stderr descriptor redirection failed")
        finally:
            for crt, pipe_fd in reversed(pipes):
                crt._close(pipe_fd)
        # Each Win32 slot borrows its own duplicate, not the Python pipe's owner.
        # A CRT loaded during evaluation can inherit these handles. Its eventual
        # _close must not invalidate the Python pipe descriptor used for draining.
        for number, _, _, handle in handles:
            if not self.kernel.SetStdHandle(number, handle):
                raise ExcitationError("external stdout/stderr process handle redirection failed")

    def restore(self, saved, handles):
        # A transient allocation failure in exit-time adoption gets one recovery
        # attempt while every original/core backup and drain handle is still live.
        # Discovery is not the publication test: refresh may already know a CRT
        # whose first attempted pair was rolled back.
        self.adopt_loaded(saved, handles)
        pairs = {(id(crt), fd) for crt, fd, _ in saved}
        if any((id(crt), fd) not in pairs for crt in self.crts for fd in (1, 2)):
            raise ExcitationError("external stdout/stderr restoration lacks a complete CRT pair")
        self.flush()
        failed = False
        inherited_handles = {crt._get_osfhandle(fd) for crt, fd, _ in saved}
        # Also handle partial setup: it can leave the inherited shared aliases
        # intact. Detach all saved targets before any restoration allocates OS
        # handles, using the same rule as successful redirection.
        for crt, fd, _ in saved:
            crt._close(fd)
        for entry in handles:
            handle = entry[3]
            if handle is not None and handle not in inherited_handles:
                failed = not self.kernel.CloseHandle(handle) or failed
            entry[3] = None
        for crt, fd, backup in reversed(saved):
            failed = crt._dup2(backup, fd) != 0 or failed
            failed = crt._close(backup) != 0 or failed
        for number, original, owner, _ in handles:
            # If a process handle belonged to a CRT descriptor, _dup2 closed the
            # old numeric handle. Borrow its restored descriptor's live handle.
            # Separately owned process handles were never closed by this guard.
            handle = owner[0]._get_osfhandle(owner[1]) if owner else original
            failed = not self.kernel.SetStdHandle(number, handle) or failed
        if failed:
            raise ExcitationError("external stdout/stderr descriptor restoration failed")

    def fatal_restore(self):
        # Do not unwind through the external wrapper's BaseException catcher,
        # nor flush untrusted CRT buffers during normal process finalization.
        self.kernel.TerminateProcess(self.kernel.GetCurrentProcess(), FATAL_OUTPUT_EXIT_CODE)
        os._exit(FATAL_OUTPUT_EXIT_CODE)


_WINDOWS_STREAMS = _WindowsStreams() if os.name == "nt" else None


@contextmanager
def guarded_output():
    """Reject stream writes and drain descriptor writes without buffering bytes."""
    # A native RPC is serialized. No user-code subprocess is launched here.
    sys.stdout.flush()
    sys.stderr.flush()
    if _WINDOWS_STREAMS:
        _WINDOWS_STREAMS.refresh()
        _WINDOWS_STREAMS.flush()
    read_fd, write_fd = os.pipe()
    saved, handles = [], []
    seen = [False]

    def drain():
        try:
            while True:
                data = os.read(read_fd, 4096)
                if not data:
                    return
                seen[0] = True
        except OSError:
            return

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    try:
        if _WINDOWS_STREAMS:
            _WINDOWS_STREAMS.redirect(write_fd, saved, handles)
        else:
            for fd in (1, 2):
                saved.append((fd, os.dup(fd)))
                os.dup2(write_fd, fd)
        sink = _RejectOutput()
        with redirect_stdout(sink), redirect_stderr(sink):
            yield
    finally:
        cleanup_error = None
        try:
            # Drain both Python and CRT buffering while output is quarantined.
            for stream in (sys.stdout, sys.stderr):
                stream.flush()
            if _WINDOWS_STREAMS:
                _WINDOWS_STREAMS.adopt_loaded(saved, handles)
                _WINDOWS_STREAMS.flush()
        except BaseException as exc:
            cleanup_error = exc
        try:
            if _WINDOWS_STREAMS:
                _WINDOWS_STREAMS.restore(saved, handles)
            else:
                for fd, backup in reversed(saved):
                    os.dup2(backup, fd)
                    os.close(backup)
        except BaseException as exc:
            if _WINDOWS_STREAMS:
                _WINDOWS_STREAMS.fatal_restore()
            cleanup_error = exc
        finally:
            os.close(write_fd)
        reader.join(timeout=1.)
        os.close(read_fd)
        if cleanup_error is not None:
            raise ExcitationError("external stdout/stderr cleanup failed") from cleanup_error
        if reader.is_alive():
            # Trusted code retained a duplicated pipe descriptor. Do not wait
            # indefinitely; the native caller remains the hard RPC time bound.
            raise ExcitationError("external function retained output descriptor")
        if seen[0]:
            raise ExcitationError("external function must not write stdout/stderr descriptors")


def environment():
    return {"pythonVersion": platform.python_version(),
            "numpyVersion": np.__version__, "scipyVersion": scipy.__version__}


def _path(value, label):
    if not isinstance(value, str) or not value or not Path(value).is_absolute():
        raise ExcitationError(f"{label} requires an absolute file path")
    # Retain the declared absolute pathname, so swapping its symlink target is
    # detected by later reads rather than following the old resolved target.
    result = Path(os.path.abspath(value))
    if not result.is_file():
        raise ExcitationError(f"{label} is not an available file")
    return result


def _sha(value):
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-fA-F]{64}", value) is None:
        raise ExcitationError("external SHA256 must be 64 hex characters")
    return value.lower()


def _bytes(path, expected):
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ExcitationError("external source/dependency unavailable") from exc
    if hashlib.sha256(data).hexdigest() != expected:
        raise ExcitationError("external source/dependency SHA256 mismatch; recompile required")
    return data


class _FrozenSources:
    """Per-function module namespaces; local imports never reuse global modules."""

    def __init__(self, source, dependencies, expected_environment):
        self.source = source
        self.dependencies = dependencies
        self.expected_environment = dict(expected_environment)
        self.modules = {}
        self.codes = {}
        self.python_files = {}
        self.local_directories = {source[0].parent, *(p.parent for p, _, _ in dependencies)}
        for path, _, data in dependencies:
            if path.suffix.lower() == ".py":
                name = path.stem
                if (not name.isidentifier() or name in self.python_files or name == source[0].stem
                        or name in sys.stdlib_module_names or name in ("numpy", "scipy", "range_workflow")):
                    raise ExcitationError("declared Python dependency requires a unique module filename")
                self.python_files[name] = (path, data)
        # Deterministic names retain rebuilt loaded-code identity. Avoid every
        # declared sibling name, including the old public-looking internal name.
        self.main_module_name = "_range_external_time_"+hashlib.sha256(
            str(source[0]).encode("utf-8")).hexdigest()
        while self.main_module_name in self.python_files:
            self.main_module_name += "_"
        self.loaded_functions = None

    def _import(self, name, globals=None, locals=None, fromlist=(), level=0):
        if not level and name in self.python_files:
            if name not in self.modules:
                path, data = self.python_files[name]
                self.load(name, path, data)
            return self.modules[name]
        root = name.split(".")[0]
        if any((directory/(root+".py")).is_file() or (directory/root/"__init__.py").is_file()
               for directory in self.local_directories):
            raise ExcitationError("local Python helper import must be declared in dependencyFiles")
        return builtins.__import__(name, globals, locals, fromlist, level)

    def load(self, name, path, data):
        module = ModuleType(name)
        module.__file__ = str(path)
        module.__dict__["__builtins__"] = dict(vars(builtins), __import__=self._import)
        self.modules[name] = module  # Permit ordinary sibling circular imports.
        code = compile(data, str(path), "exec", dont_inherit=True)
        self.codes[name] = hashlib.sha256(marshal.dumps(code, 2)).hexdigest()
        # Standard decorators (dataclasses with string annotations, for example)
        # inspect their defining module during execution. Register only while
        # loading and restore any global import; evaluations use scoped imports.
        missing = object()
        previous = sys.modules.get(name, missing)
        sys.modules[name] = module
        try:
            exec(code, module.__dict__)
        finally:
            if previous is missing:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous
        return module

    def functions(self):
        result = {}
        for name, module in self.modules.items():
            visited_classes = set()
            def visit(key, value):
                if isinstance(value, (staticmethod, classmethod)):
                    value = value.__func__
                if inspect.isfunction(value) and value.__globals__ is vars(module):
                    result[(name, key)] = hashlib.sha256(marshal.dumps(value.__code__, 2)).hexdigest()
                elif inspect.isclass(value) and value.__module__ == module.__name__:
                    if id(value) in visited_classes:
                        return
                    visited_classes.add(id(value))
                    for member, method in vars(value).items():
                        visit(key+"."+member, method)
                elif isinstance(value, property):
                    for suffix, method in (("get", value.fget), ("set", value.fset), ("del", value.fdel)):
                        if method is not None:
                            visit(key+"."+suffix, method)
            for key, value in vars(module).items():
                visit(key, value)
        return result

    def check(self):
        try:
            if _SOURCE_PATH.read_bytes() != _SOURCE_BYTES:
                raise ExcitationError("external loader source changed; reload required")
        except OSError as exc:
            raise ExcitationError("external loader source unavailable") from exc
        if environment() != self.expected_environment:
            raise ExcitationError("external environment version mismatch; reload required")
        for path, sha, _ in (self.source, *self.dependencies):
            _bytes(path, sha)
        if self.loaded_functions is not None and self.functions() != self.loaded_functions:
            raise ExcitationError("external loaded dependency/function code changed; reload required")


def load_external_functions(records, catalog):
    """Load the frozen JSON externalFunctions contract, returning wrappers."""
    if not isinstance(records, (list, tuple)):
        raise ExcitationError("externalFunctions must be an array")
    by_id = {c.channel_id: c for c in catalog}
    seen_channels, seen_functions = set(), set()
    wrappers = []
    for record in records:
        _keys(record, ("sourcePath", "functionName", "sourceSha256", "dependencyFiles",
                       "environment", "channels", "missingPolicy", "cacheExactTime"))
        path = _path(record["sourcePath"], "sourcePath")
        name = record["functionName"]
        if path.suffix.lower() != ".py" or not isinstance(name, str) or not name.isidentifier():
            raise ExcitationError("external source must be .py with a function identifier")
        if (path, name) in seen_functions:
            raise ExcitationError("duplicate external function")
        seen_functions.add((path, name))
        sha = _sha(record["sourceSha256"])
        source = (path, sha, _bytes(path, sha))
        if not isinstance(record["dependencyFiles"], (list, tuple)):
            raise ExcitationError("dependencyFiles must be an array")
        dependencies, paths = [], {path}
        for entry in record["dependencyFiles"]:
            _keys(entry, ("path", "sha256"))
            dep = _path(entry["path"], "dependency path")
            if dep in paths:
                raise ExcitationError("duplicate source/dependency path")
            paths.add(dep)
            dep_sha = _sha(entry["sha256"])
            dependencies.append((dep, dep_sha, _bytes(dep, dep_sha)))
        _keys(record["environment"], ("pythonVersion", "numpyVersion", "scipyVersion"))
        if record["environment"] != environment():
            raise ExcitationError("external environment version mismatch")
        if not isinstance(record["channels"], (list, tuple)) or not record["channels"]:
            raise ExcitationError("external channels must be nonempty array")
        channels = []
        for entry in record["channels"]:
            _keys(entry, ("channelId", "unit"))
            cid = entry["channelId"]
            if not isinstance(cid, str) or cid not in by_id or cid in seen_channels:
                raise ExcitationError("unknown or duplicate external channel")
            if entry["unit"] != by_id[cid].unit:
                raise ExcitationError("external declared force unit mismatch")
            channels.append(by_id[cid])
            seen_channels.add(cid)
        frozen = _FrozenSources(source, tuple(dependencies), record["environment"])
        frozen.check()
        try:
            with guarded_output():
                module = frozen.load(frozen.main_module_name, path, source[2])
                # Preload declared modules to freeze lazy imports too.
                for dep_name, (dep_path, data) in frozen.python_files.items():
                    if dep_name not in frozen.modules:
                        frozen.load(dep_name, dep_path, data)
        except ExcitationError:
            raise
        except BaseException as exc:
            raise ExcitationError("external source load failed") from exc
        frozen.loaded_functions = frozen.functions()
        frozen.check()
        function = vars(module).get(name)
        wrappers.append(ExternalTimeFunction(function,
            code_identity=_digest({"source": sha, "function": name, "loaded": frozen.codes,
                                   "loader": _SOURCE_HASH, "loaderCode": _CODE_HASH}),
            dependency_identity=_digest([[str(p), s] for p, s, _ in dependencies]),
            environment_identity=_digest(record["environment"]), channels=tuple(channels),
            missing_policy=record["missingPolicy"], cache_exact_time=record["cacheExactTime"],
            revision_check=frozen.check))
    return tuple(wrappers)
