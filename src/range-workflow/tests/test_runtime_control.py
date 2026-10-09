"""Real local socket checks for bounded correlated native runtime messages."""
import copy
import json
from pathlib import Path
import queue
import socket
import sys
import threading
import time
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.runtime_control import RuntimeControlSession, SCHEMA
from range_workflow import runtime_parameters as rp


class RuntimeControlTest(unittest.TestCase):
    def setUp(self):
        self.events, self.cancel = queue.Queue(), threading.Event()
        self.ids = [str(uuid.uuid4()) for _ in range(3)]
        self.session = RuntimeControlSession(*self.ids, "a" * 64, 1, self.events.put, self.cancel)
        env = self.session.environment()
        self.peer = socket.create_connection(("127.0.0.1", int(env["RANGE_RUNTIME_CONTROL_PORT"])), timeout=2)
        self.peer.settimeout(2)
        self.stream = self.peer.makefile("rb")
        self.identity = dict(zip(("jobId", "runId", "taskId"), self.ids),
                             modelSha256="a" * 64, baseControllerRevision="b" * 64)
        self.limits = [{"actuatorId": "f", "minN": -10, "maxN": 10, "rateNPerS": 5}]
        self.initial = {"schema": rp.SCHEMA, "parameters": rp.make_parameters("b" * 64, 0, 1, self.limits), "history": []}
        self.hello = {"schema": SCHEMA, "op": "hello", "requestId": str(uuid.uuid4()),
                      "identity": self.identity, "token": env["RANGE_RUNTIME_CONTROL_TOKEN"],
                      "acceptedState": self.initial}

    def tearDown(self):
        self.session.close()
        self.stream.close()
        self.peer.close()

    def send(self, frame):
        self.peer.sendall(json.dumps(frame, allow_nan=False).encode() + b"\n")

    def handshake(self):
        self.send(self.hello)
        ready = json.loads(self.stream.readline())
        self.assertEqual(ready["op"], "ready")
        self.assertEqual(ready["requestId"], self.hello["requestId"])
        self.assertEqual(self.events.get(timeout=2)["op"], "ready")

    def test_correlated_queued_then_authoritative_acceptance(self):
        self.handshake()
        proposal = str(uuid.uuid4())
        parameters = rp.candidate(self.initial, 2, self.limits, self.limits)
        request_id = self.session.request("enqueue", {"proposalId": proposal,
            "expectedParameterRevision": "0", "parameters": parameters})
        request = json.loads(self.stream.readline())
        self.assertEqual(request["requestId"], request_id)
        receipt = {"proposalId": proposal, "parameterRevision": "1", "parameterSha256": parameters["parameterSha256"],
                   "runtimeControllerRevision": parameters["runtimeControllerRevision"], "status": "queued",
                   "reason": "none", "provenance": "currentAttempt"}
        self.send({"schema": SCHEMA, "op": "reply", "requestId": request_id, "identity": self.identity,
                   "command": "enqueue", "ok": True, "reason": "none", "receipt": receipt})
        self.assertEqual(self.events.get(timeout=2)["receipt"]["status"], "queued")
        self.assertEqual(self.session.snapshot()["acceptedState"], self.initial)
        accepted = {"schema": rp.SCHEMA, "parameters": parameters, "history": [{"proposalId": proposal,
            "previousRevision": "0", "revision": "1", "appliedSeq": "17", "acceptedTimeS": .25,
            "parameterSha256": parameters["parameterSha256"]}]}
        receipt.update(status="accepted", appliedSeq="17", acceptedTimeS=.25)
        self.send({"schema": SCHEMA, "op": "receipt", "requestId": str(uuid.uuid4()),
                   "identity": self.identity, "receipt": receipt, "acceptedState": accepted})
        self.assertEqual(self.events.get(timeout=2)["acceptedState"], accepted)
        self.assertEqual(self.session.snapshot()["acceptedState"], accepted)
        self.assertFalse(self.cancel.is_set())

    def test_wrong_token_disconnects_and_grants_no_ready_authority(self):
        self.hello["token"] = "c" * 64
        self.send(self.hello)
        self.assertTrue(self.cancel.wait(2))
        self.assertIsNone(self.session.snapshot()["identity"])
        self.assertEqual(self.events.get(timeout=2)["op"], "disconnected")

    def test_duplicate_escaped_field_rejected(self):
        raw = json.dumps(self.hello).encode()
        raw = raw[:-1] + b',"sch\\u0065ma":"range.runtime-control/v1"}'
        self.peer.sendall(raw + b"\n")
        self.assertTrue(self.cancel.wait(2))
        self.assertIsNone(self.session.snapshot()["acceptedState"])

    def test_uncorrelated_reply_cancels_without_claiming_rejection(self):
        self.handshake()
        self.send({"schema": SCHEMA, "op": "reply", "requestId": str(uuid.uuid4()),
                   "identity": self.identity, "command": "resume", "ok": True, "reason": "none"})
        self.assertTrue(self.cancel.wait(2))
        self.assertEqual(self.events.get(timeout=2)["op"], "disconnected")
        self.assertEqual(self.session.snapshot()["acceptedState"], self.initial)

    def test_oversized_unterminated_frame_cancels(self):
        self.peer.sendall(b"x" * 65537)
        self.assertTrue(self.cancel.wait(2))

    def test_pinned_initial_gain_mismatch_rejected(self):
        self.hello["acceptedState"]["parameters"] = rp.make_parameters("b" * 64, 0, 3, self.limits)
        self.send(self.hello)
        self.assertTrue(self.cancel.wait(2))
        self.assertIsNone(self.session.snapshot()["acceptedState"])


if __name__ == "__main__":
    unittest.main()
