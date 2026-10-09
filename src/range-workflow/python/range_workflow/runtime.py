"""Persistent JSON-lines entry point shared by the GUI and Python clients."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
import sys
import threading
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from .service import (WorkflowError, WorkflowService, MAX_REQUEST_BYTES, READ_OPERATIONS,
                      MAX_READ_QUERIES, RUNTIME_VERIFICATION_QUERY)


def _absolute_file(value: str, label: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or len(value) > 32768 or not path.is_file():
        raise argparse.ArgumentTypeError(f"--{label} must be an existing absolute file path")
    return path.resolve()


def _absolute_dir(value: str, label: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or len(value) > 32768 or not path.is_dir():
        raise argparse.ArgumentTypeError(f"--{label} must be an existing absolute directory path")
    return path.resolve()


def _arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdio", action="store_true", required=True,
                        help="serve versioned JSON-lines requests on stdin/stdout")
    parser.add_argument("--workspace", required=True, help="absolute writable project workspace")
    parser.add_argument("--solver", required=True, help="absolute fea-solver executable")
    parser.add_argument("--native", required=True, help="absolute range-workflow-native executable")
    parser.add_argument("--native-results", help="absolute native results broker executable")
    parser.add_argument("--examples", required=True, help="absolute installed/source examples directory")
    parser.add_argument("--python", required=True, help="absolute Python interpreter executable")
    parser.add_argument("--trusted-extensions", action="store_true",
                        help="allow host-approved extension Python in trusted developer mode; not an OS sandbox")
    result = parser.parse_args(argv)
    if result.native_results:
        try:
            result.native_results = _absolute_file(result.native_results, "native-results")
        except argparse.ArgumentTypeError as error:
            parser.error(str(error))
    result.workspace = Path(result.workspace)
    if not result.workspace.is_absolute() or len(str(result.workspace)) > 32768:
        parser.error("--workspace must be an absolute path")
    result.workspace = result.workspace.resolve()
    result.workspace.mkdir(parents=True, exist_ok=True)
    for key in ("solver", "native", "python"):
        value = getattr(result, key)
        try:
            setattr(result, key, _absolute_file(value, key))
        except argparse.ArgumentTypeError as error:
            parser.error(str(error))
    try:
        result.examples = _absolute_dir(result.examples, "examples")
    except argparse.ArgumentTypeError as error:
        parser.error(str(error))
    return result


def _write_response(output: Any, request_id: Any, status: str,
                    code: str, result: dict[str, Any]) -> None:
    envelope = {"apiVersion": "range.workflow/v1", "requestId": request_id,
                "status": status, "code": code, "result": result}
    data = (json.dumps(envelope, ensure_ascii=False, allow_nan=False,
                       separators=(",", ":")) + "\n").encode("utf-8")
    output.write(data)
    output.flush()


def serve(service: WorkflowService, stdin: Any = None, stdout: Any = None) -> int:
    source = stdin if stdin is not None else sys.stdin.buffer
    target = stdout if stdout is not None else sys.stdout.buffer
    output_lock = threading.Lock()
    capacity = threading.BoundedSemaphore(MAX_READ_QUERIES)
    executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="workflow-read")

    def respond(request_id, status, code, result):
        with output_lock:
            _write_response(target, request_id, status, code, result)

    def read_reply(query, request_id):
        try:
            respond(request_id, "ok", "OK", query.run())
        except WorkflowError as error:
            respond(request_id, "error", error.code, {"message": error.message})
        except Exception:
            respond(request_id, "error", "INTERNAL_ERROR",
                    {"message": "Workflow service could not complete the request"})
        finally:
            capacity.release()

    try:
        return _serve_lines(service, source, respond, executor, capacity, read_reply)
    finally:
        # EOF disconnects this host. Stop native children before joining the two
        # readers; queued views run only their cancellation check and release.
        service.cancel_reads()
        service.cancel_runtime_verifications()
        executor.shutdown(wait=True)


def _serve_lines(service, source, respond, executor, capacity, read_reply):
    while True:
        line = source.readline(MAX_REQUEST_BYTES + 1)
        if not line:
            return 0
        if len(line) > MAX_REQUEST_BYTES:
            while line and not line.endswith(b"\n"):
                line = source.readline(65536)
            respond(None, "error", "REQUEST_TOO_LARGE",
                            {"message": "Request line exceeds the configured byte limit"})
            continue
        if line.endswith(b"\n"):
            line = line[:-1]
        try:
            request = json.loads(line.decode("utf-8", "strict"),
                                 parse_constant=lambda _value: (_ for _ in ()).throw(
                                     ValueError("non-finite number")),
                                 object_pairs_hook=_unique_object)
        except (UnicodeError, ValueError, json.JSONDecodeError, RecursionError):
            respond(None, "error", "INVALID_JSON",
                            {"message": "Request line is not valid strict UTF-8 JSON"})
            continue
        request_id = request.get("requestId") if isinstance(request, dict) else None
        if not isinstance(request_id, str) or len(request_id) > 128:
            request_id = None
        try:
            if (isinstance(request, dict) and request.get("operation") == RUNTIME_VERIFICATION_QUERY):
                # The bounded environment stamp may read a manifest. Keep that IO
                # off the stdio owner loop, just like the completed-result read lane.
                if not capacity.acquire(blocking=False):
                    raise WorkflowError("READ_QUERY_BUSY", "The bounded read lane is full")
                # Capture per-request values: the input loop immediately advances.
                query = SimpleNamespace(run=lambda held=request: service.handle(held))
                try:
                    executor.submit(read_reply, query, request_id)
                except BaseException:
                    capacity.release()
                    raise
                continue
            if (isinstance(request, dict) and isinstance(request.get("operation"), str)
                    and request["operation"] in READ_OPERATIONS):
                query = service.prepare_read(request)
                if not capacity.acquire(blocking=False):
                    query.reject_busy()
                try:
                    executor.submit(read_reply, query, request_id)
                except BaseException:
                    capacity.release()
                    raise
                continue
            response = service.handle(request)
            respond(request_id, "ok", "OK", response)
        except WorkflowError as error:
            respond(request_id, "error", error.code,
                            {"message": error.message})
        except Exception:
            # Keep implementation details and untrusted input out of protocol diagnostics.
            respond(request_id, "error", "INTERNAL_ERROR",
                            {"message": "Workflow service could not complete the request"})


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def main(argv: list[str] | None = None) -> int:
    arguments = _arguments(argv)
    try:
        with WorkflowService(arguments.workspace, arguments.solver,
                             arguments.native, arguments.examples,
                             arguments.python,
                             trusted_extensions=arguments.trusted_extensions,
                             native_results=arguments.native_results) as service:
            return serve(service)
    except WorkflowError as error:
        print(f"range-workflow: {error.code}: {error.message}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"range-workflow: cannot open workspace: {error}", file=sys.stderr)
        return 2
