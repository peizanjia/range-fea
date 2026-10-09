"""Pure stdio SDK isolation tests: no solver, Qt, or native executable."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import WorkflowClient, WorkflowError
from range_workflow import client as client_module


FIXTURE = r'''
import json, subprocess, sys, threading, time
lock = threading.Lock()
pending = {}
workers = []
children = []
def reply(request, result=None, code="OK", raw=None):
    message = {"apiVersion":"range.workflow/v1", "requestId":request["requestId"],
        "status":"ok" if code == "OK" else "error", "code":code, "result":result or {}}
    with lock:
        print(raw if raw is not None else json.dumps(message), flush=True)
def delayed(request):
    params = request["parameters"]
    if params.get("childMarker"):
        child = pending[request["requestId"]]["_child"]
        while child.poll() is None:
            time.sleep(.01)
    else:
        time.sleep(params.get("delay", .3))
    if params.get("lateBad"):
        reply(request, raw=json.dumps({"requestId":request["requestId"], "result":{}}))
    else:
        reply(request, {"tag":params.get("tag", "late")})
def delayed_receipt(request, result):
    time.sleep(.18)
    reply(request, result)
for line in sys.stdin:
    request = json.loads(line)
    op, params = request["operation"], request["parameters"]
    if op == "capabilities.query":
        reply(request, {"apiVersion":"range.workflow/v1"})
    elif op == "host.read.cancel":
        target = params["requestId"]
        selected = pending.get(target, {})
        policy = selected.get("cancelPolicy", "true")
        if policy == "silent":
            continue
        if policy == "error":
            reply(request, {"message":"fixture refused cancellation"}, "READ_CANCEL_FAILED")
        elif policy in ("late", "lateWrong"):
            result = {"requestId":target if policy == "late" else request["requestId"], "readCancelled":True}
            worker = threading.Thread(target=delayed_receipt, args=(request, result))
            workers.append(worker)
            worker.start()
        else:
            reply(request, {"requestId":target if policy != "wrong" else request["requestId"],
                "readCancelled": policy != "false"})
    elif params.get("bad") == "unknown":
        request["requestId"] = "00000000-0000-4000-8000-000000000000"
        reply(request)
    elif params.get("bad") == "schema":
        reply(request, raw=json.dumps({"requestId":request["requestId"], "result":{}}))
    elif params.get("bad") == "duplicate":
        raw = json.dumps({"apiVersion":"range.workflow/v1", "requestId":request["requestId"],
            "status":"ok", "code":"OK", "result":{}})
        reply(request, raw=raw[:-1] + ',"code":"OK"}')
    elif params.get("bad") == "oversize":
        reply(request, {"payload":"x" * (16 * 1024 * 1024)})
    elif params.get("delay") is not None or params.get("childMarker"):
        pending[request["requestId"]] = params
        if params.get("childMarker"):
            child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            children.append((child, params["childMarker"]))
            params["_child"] = child
        worker = threading.Thread(target=delayed, args=(request,))
        workers.append(worker)
        worker.start()
    else:
        reply(request, {"tag":params.get("tag", "control")})
for child, marker in children:
    child.terminate()
    child.wait(timeout=3)
    with open(marker, "w") as stream:
        stream.write("child reaped")
for worker in workers:
    worker.join()
'''


class ReadTransportTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        script = self.base / "fixed_stdio.py"
        script.write_text(FIXTURE, encoding="utf-8")
        real_popen = subprocess.Popen
        def fixture_popen(_args, **kwargs):
            return real_popen([sys.executable, "-u", str(script)], **kwargs)
        self.patch = mock.patch("range_workflow.client.subprocess.Popen", side_effect=fixture_popen)
        self.patch.start()
        self.client = WorkflowClient(self.base, sys.executable, sys.executable, self.base, timeout=1)

    def tearDown(self):
        self.client.close()
        self.patch.stop()
        self.temporary.cleanup()

    def wait_until(self, predicate, timeout=3):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() > deadline:
                self.fail("fixture condition did not become true")
            time.sleep(.005)

    def test_out_of_order_reads_and_fast_runtime_reply(self):
        self.client.timeout = .3
        with ThreadPoolExecutor(max_workers=3) as pool:
            slow = pool.submit(self.client.request, "result.plot.query", {"delay":.2, "tag":"slow"})
            self.wait_until(lambda: len(self.client._pending) == 1)
            faster = pool.submit(self.client.request, "result.raw.query", {"delay":.06, "tag":"faster"})
            control = pool.submit(self.client.request, "runtime.parameters.query", {"tag":"live"})
            self.assertEqual(control.result(timeout=.15), {"tag":"live"})
            self.assertFalse(slow.done())
            self.assertEqual(faster.result(timeout=.15), {"tag":"faster"})
            self.assertEqual(slow.result(timeout=.3), {"tag":"slow"})

    def test_read_timeout_cancels_individually_and_valid_late_reply_is_ignored(self):
        self.client.timeout = .08
        with self.assertRaises(WorkflowError) as captured:
            self.client.request("result.raw.export", {"delay":.18})
        self.assertEqual(captured.exception.code, "SDK_TIMEOUT")
        self.wait_until(lambda: bool(self.client._cancel_outcomes))
        self.assertEqual(list(self.client._cancel_outcomes.values()), [True])
        self.assertEqual(self.client.request("job.cancel"), {"tag":"control"})
        self.wait_until(lambda: not self.client._retired)
        self.assertFalse(self.client._closed)
        self.assertEqual(self.client.request("runtime.parameters.query"), {"tag":"control"})

    def test_cancel_false_and_error_do_not_assert_success_or_retry(self):
        self.client.timeout = .08
        for policy, expected in (("false", False), ("error", "READ_CANCEL_FAILED")):
            with self.assertRaises(WorkflowError):
                self.client.request("result.raw.query", {"delay":.13, "cancelPolicy":policy})
            self.wait_until(lambda: expected in self.client._cancel_outcomes.values())
            self.assertFalse(self.client._closed)
        self.wait_until(lambda: not self.client._retired)

    def test_unknown_malformed_and_duplicate_responses_fail_closed(self):
        for malformed in ("unknown", "schema", "duplicate"):
            with self.subTest(malformed=malformed):
                if self.client._closed:
                    self.client.close()
                    self.client = WorkflowClient(self.base, sys.executable, sys.executable, self.base, timeout=1)
                with self.assertRaises(WorkflowError) as captured:
                    self.client.request("project.query", {"bad":malformed})
                self.assertEqual(captured.exception.code, "INVALID_RESPONSE")
                self.assertTrue(self.client._closed)

    def test_mutation_timeout_still_fails_all_inflight_and_closes(self):
        self.client.timeout = .1
        with ThreadPoolExecutor(max_workers=2) as pool:
            mutation = pool.submit(self.client.request, "command.execute", {"delay":1})
            self.wait_until(lambda: len(self.client._pending) == 1)
            self.client.timeout = .5
            read = pool.submit(self.client.request, "result.raw.query", {"delay":1})
            for future in (mutation, read):
                with self.assertRaises(WorkflowError) as captured:
                    future.result(timeout=3)
                self.assertEqual(captured.exception.code, "SDK_TIMEOUT")
        self.assertTrue(self.client._closed)
        self.assertIsNotNone(self.client.process.poll())
        self.assertFalse(self.client._cancel_outcomes)

    def test_cancel_receipt_has_finite_deadline_and_strict_target(self):
        self.client.timeout = .06
        with self.assertRaises(WorkflowError):
            self.client.request("result.plot.query", {"delay":.4, "cancelPolicy":"silent"})
        self.wait_until(lambda: bool(self.client._cancel_outcomes))
        self.assertEqual(list(self.client._cancel_outcomes.values()), ["SDK_TIMEOUT"])
        self.assertFalse(self.client._closed)
        self.assertEqual(self.client.request("runtime.parameters.query"), {"tag":"control"})
        self.client.close()
        self.client = WorkflowClient(self.base, sys.executable, sys.executable, self.base, timeout=1)
        self.client.timeout = .06
        with self.assertRaises(WorkflowError):
            self.client.request("result.plot.query", {"delay":.4, "cancelPolicy":"wrong"})
        self.wait_until(lambda: self.client._closed)
        self.assertEqual(list(self.client._cancel_outcomes.values()), ["INVALID_RESPONSE"])

    def test_close_fails_waiter_and_owns_child_shutdown(self):
        marker = self.base / "child-closed.txt"
        self.client.timeout = 5
        with ThreadPoolExecutor(max_workers=1) as pool:
            read = pool.submit(self.client.request, "result.plot.query", {"childMarker":str(marker)})
            self.wait_until(lambda: len(self.client._pending) == 1)
            # A fast round trip proves the service received the preceding read.
            self.client.request("runtime.parameters.query")
            self.client.close()
            with self.assertRaises(WorkflowError) as captured:
                read.result(timeout=1)
            self.assertEqual(captured.exception.code, "SERVICE_CLOSED")
        self.assertEqual(marker.read_text(), "child reaped")
        self.assertIsNotNone(self.client.process.poll())
        self.assertFalse(self.client._reader.is_alive())
        self.assertFalse(self.client._error_reader.is_alive())

    def test_late_cancel_ack_is_validated_and_does_not_claim_success(self):
        self.client.timeout = .06
        with self.assertRaises(WorkflowError):
            self.client.request("result.plot.query", {"delay":.35, "cancelPolicy":"late"})
        self.wait_until(lambda: bool(self.client._cancel_outcomes))
        self.assertEqual(list(self.client._cancel_outcomes.values()), ["SDK_TIMEOUT"])
        self.assertEqual(self.client.request("runtime.parameters.query"), {"tag":"control"})
        self.wait_until(lambda: not self.client._retired)
        self.assertFalse(self.client._closed)
        self.assertEqual(list(self.client._cancel_outcomes.values()), ["SDK_TIMEOUT"])

    def test_malformed_known_late_reply_and_late_ack_fail_closed(self):
        self.client.timeout = .06
        with self.assertRaises(WorkflowError):
            self.client.request("result.plot.query", {"delay":.15, "lateBad":True})
        self.wait_until(lambda: self.client._closed)
        self.client.close()
        self.client = WorkflowClient(self.base, sys.executable, sys.executable, self.base, timeout=1)
        self.client.timeout = .06
        with self.assertRaises(WorkflowError):
            self.client.request("result.plot.query", {"delay":.35, "cancelPolicy":"lateWrong"})
        self.wait_until(lambda: bool(self.client._cancel_outcomes))
        self.assertFalse(self.client._closed)
        self.wait_until(lambda: self.client._closed)

    def test_inflight_and_request_byte_limits_are_bounded(self):
        self.client.timeout = 5
        with ThreadPoolExecutor(max_workers=16) as pool:
            reads = [pool.submit(self.client.request, "result.raw.query", {"delay":.5}) for _ in range(16)]
            self.wait_until(lambda: len(self.client._pending) == 16)
            with self.assertRaises(WorkflowError) as captured:
                self.client.request("runtime.parameters.query")
            self.assertEqual(captured.exception.code, "SERVICE_BUSY")
            self.assertEqual([item.result(timeout=2) for item in reads], [{"tag":"late"}] * 16)
        with self.assertRaises(WorkflowError) as captured:
            self.client.request("result.raw.query", {"oversized":"x" * (256 * 1024)})
        self.assertEqual(captured.exception.code, "REQUEST_TOO_LARGE")
        self.assertFalse(self.client._closed)

    def test_response_byte_limit_fails_closed(self):
        with self.assertRaises(WorkflowError) as captured:
            self.client.request("project.query", {"bad":"oversize"})
        self.assertEqual(captured.exception.code, "RESPONSE_TOO_LARGE")
        self.assertTrue(self.client._closed)

    def test_response_routed_at_timeout_race_does_not_close_healthy_transport(self):
        original_pending = client_module._Pending
        class DeadlineRaceQueue(client_module.queue.Queue):
            def get(self, block=True, timeout=None):
                if timeout is not None:
                    # Inject Queue.get's deadline race: the reader has already
                    # removed the context and queued its valid response by the
                    # time the caller acquires the registry lock after Empty.
                    response = super().get(block, timeout)
                    self.put_nowait(response)
                    raise client_module.queue.Empty
                return super().get(block, timeout)
        def pending_with_race(operation, cancel_target=None):
            pending = original_pending(operation, cancel_target)
            pending.responses = DeadlineRaceQueue(maxsize=1)
            return pending
        with mock.patch("range_workflow.client._Pending", side_effect=pending_with_race):
            self.assertEqual(self.client.request("project.query"), {"tag":"control"})
        self.assertFalse(self.client._closed)
        self.assertIsNone(self.client.process.poll())
        self.assertEqual(self.client.request("runtime.parameters.query"), {"tag":"control"})

    def assert_reader_progress_during_blocked_send(self, blocked_operation, first_operation, first_delay):
        entered, release = threading.Event(), threading.Event()
        original_stdin = self.client.process.stdin
        class BlockedPipe:
            def write(self, raw):
                if json.loads(raw)["operation"] == blocked_operation:
                    entered.set()
                    if not release.wait(3):
                        raise OSError("blocked fixture sender was not released")
                return original_stdin.write(raw)
            def flush(self):
                return original_stdin.flush()
            def close(self):
                return original_stdin.close()
        self.client.process.stdin = BlockedPipe()
        with ThreadPoolExecutor(max_workers=2) as pool:
            self.client.timeout = .5
            first = pool.submit(self.client.request, first_operation, {"delay":first_delay})
            self.wait_until(lambda: len(self.client._pending) == 1)
            self.client.timeout = .08 if blocked_operation == "host.read.cancel" else .5
            second = pool.submit(self.client.request, "result.raw.query", {"delay":.2})
            try:
                self.assertTrue(entered.wait(1), "fixture write was not blocked")
                # This reply requires the actual SDK reader to acquire its
                # routing lock while the other thread remains inside write().
                self.assertEqual(first.result(timeout=.3), {"tag":"late"})
                self.assertFalse(release.is_set())
                self.assertFalse(self.client._closed)
            finally:
                release.set()
            if blocked_operation == "host.read.cancel":
                with self.assertRaises(WorkflowError) as captured:
                    second.result(timeout=1)
                self.assertEqual(captured.exception.code, "SDK_TIMEOUT")
                self.wait_until(lambda: bool(self.client._cancel_outcomes))
                self.assertEqual(list(self.client._cancel_outcomes.values()), [True])
                self.wait_until(lambda: not self.client._retired)
            else:
                self.assertEqual(second.result(timeout=1), {"tag":"late"})
        self.assertEqual(self.client.request("runtime.parameters.query"), {"tag":"control"})

    def test_request_pipe_write_does_not_block_reply_routing(self):
        self.assert_reader_progress_during_blocked_send("result.raw.query", "result.plot.query", .1)

    def test_cancel_pipe_write_does_not_block_reply_routing(self):
        self.assert_reader_progress_during_blocked_send("host.read.cancel", "project.query", .13)


if __name__ == "__main__":
    unittest.main()
