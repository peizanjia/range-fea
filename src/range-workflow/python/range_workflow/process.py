"""Bounded subprocess execution, including descendant cleanup on Windows."""
from __future__ import annotations

import ctypes
import os
from pathlib import Path
import signal
import subprocess
import threading
import time


class ProcessFailure(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


class _WindowsJob:
    def __init__(self, process: subprocess.Popen):
        from ctypes import wintypes
        class Basic(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                        ("PerJobUserTimeLimit", ctypes.c_int64), ("LimitFlags", wintypes.DWORD),
                        ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t),
                        ("ActiveProcessLimit", wintypes.DWORD), ("Affinity", ctypes.c_size_t),
                        ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]
        class IO(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                         "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]
        class Extended(ctypes.Structure):
            _fields_ = [("BasicLimitInformation", Basic), ("IoInfo", IO),
                        ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = wintypes.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        self.kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "Cannot create process job")
        info = Extended()
        info.BasicLimitInformation.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(info), ctypes.sizeof(info)):
            self.close()
            raise OSError(ctypes.get_last_error(), "Cannot configure process job")
        if not self.kernel.AssignProcessToJobObject(self.handle, wintypes.HANDLE(int(process._handle))):
            self.close()
            raise OSError(ctypes.get_last_error(), "Cannot isolate subprocess tree")
        # Popen starts suspended: the process cannot spawn before assignment.
        resume = ctypes.WinDLL("ntdll").NtResumeProcess
        resume.argtypes = [wintypes.HANDLE]
        resume.restype = ctypes.c_long
        if resume(wintypes.HANDLE(int(process._handle))) != 0:
            self.close()
            raise OSError("Cannot resume isolated process")

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class ProcessRunner:
    MAX_LOG_BYTES = 8 * 1024 * 1024

    def run(self, argv: list[str], directory: Path, label: str,
            cancel: threading.Event, timeout: float = 180, env: dict | None = None) -> bytes:
        if cancel.is_set():
            raise ProcessFailure("CANCELLED", "Job was cancelled")
        log = directory / (label + ".log")
        out = bytearray()
        overflow = threading.Event()
        failure = []
        flags = {"creationflags": 0x4 | 0x08000000} if os.name == "nt" else {"start_new_session": True}
        process = subprocess.Popen([str(x) for x in argv], cwd=directory, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, **flags)
        job = None
        try:
            if os.name == "nt":
                job = _WindowsJob(process)
            def drain():
                try:
                    with log.open("xb") as stream:
                        while True:
                            block = process.stdout.read(65536)
                            if not block:
                                break
                            room = self.MAX_LOG_BYTES - len(out)
                            stream.write(block[:room])
                            out.extend(block[:room])
                            if len(block) > room:
                                overflow.set()
                                break
                except OSError as error:
                    failure.append(error)
                    overflow.set()
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()
            deadline = time.monotonic() + timeout
            while process.poll() is None:
                if cancel.is_set():
                    raise ProcessFailure("CANCELLED", "Job was cancelled")
                if overflow.is_set():
                    raise ProcessFailure("OUTPUT_LIMIT", "Process output exceeded its bound or could not be saved")
                if time.monotonic() >= deadline:
                    raise ProcessFailure("TIMEOUT", "Process exceeded its time limit")
                cancel.wait(0.025)
            # Closing the job also closes descendant-owned copies of the pipe.
            if job:
                job.close()
            elif os.name != "nt":
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            reader.join(5)
            if reader.is_alive() or overflow.is_set() or failure:
                raise ProcessFailure("OUTPUT_LIMIT", "Process output exceeded its bound or could not be saved")
            if cancel.is_set():
                raise ProcessFailure("CANCELLED", "Job was cancelled")
            if process.returncode:
                raise ProcessFailure("PROCESS_FAILED", f"{label} exited with code {process.returncode}; see job log")
            return bytes(out)
        finally:
            if job:
                job.close()
            if process.poll() is None:
                if os.name == "nt":
                    process.kill()
                else:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            process.wait(timeout=5)
            if 'reader' in locals():
                reader.join(5)
            process.stdout.close()
