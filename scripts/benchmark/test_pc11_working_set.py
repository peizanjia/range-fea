"""Focused mock checks plus an opt-in read-only Windows self-PID probe."""
import importlib.util
import math
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import json

spec = importlib.util.spec_from_file_location("pc11", Path(__file__).with_name("pc11_working_set.py"))
pc11 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc11)


class Fake:
    def __init__(self):
        self.tree = {1: 0, 2: 1, 3: 2}
        self.identities = {1: (10, pc11.canonical("gui.exe")),
                           2: (11, pc11.canonical("compute/worker.exe")),
                           3: (12, pc11.canonical("compute/python.exe"))}
        self.fail_open = set(); self.fail_memory = set(); self.dead = set(); self.closed = []
    def enumerate(self):
        return {pid: parent for pid, parent in self.tree.items() if pid not in self.dead}
    def open(self, pid):
        if pid in self.fail_open:
            raise OSError("denied")
        return pid
    def close(self, handle):
        self.closed.append(handle)
    def identity(self, handle):
        return self.identities[handle]
    def exited(self, handle):
        return handle in self.dead
    def wait_exited(self, handle, timeout_ms):
        return self.exited(handle)
    def times(self, handle):
        return self.identities[handle][0], (100 + handle if handle in self.dead else 0)
    def working_set(self, handle):
        if handle in self.fail_memory:
            raise OSError("denied")
        return 100 * handle


class Tests(unittest.TestCase):
    def make(self, api):
        sampler = pc11.Sampler(api, 1, "gui.exe", ["compute"], lambda event: None)
        self.addCleanup(sampler.close)
        return sampler

    def test_sum_includes_root_and_grandchild(self):
        sampler = self.make(Fake())
        self.assertEqual(sampler.sample()[0], 600)
        self.assertEqual(len(sampler.proof()), 3)
        self.assertEqual(sampler.proof()[2]["parentCreationTime100ns"], 11)

    def test_reused_root_rejected(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        api.identities[1] = (20, pc11.canonical("gui.exe"))
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "reused"):
            sampler.sample()

    def test_reused_descendant_rejected(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        api.identities[2] = (20, pc11.canonical("compute/worker.exe"))
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "descendant PID reused"):
            sampler.sample()

    def test_alive_unreadable_child_open_rejected(self):
        api = Fake(); api.fail_open.add(2)
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "unreadable"):
            self.make(api).sample()

    def test_alive_unreadable_memory_rejected(self):
        api = Fake(); api.fail_memory.add(2)
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "unreadable"):
            self.make(api).sample()

    def test_exited_child_query_skipped(self):
        api = Fake(); sampler = self.make(api); sampler.sample(); api.dead.add(2)
        self.assertEqual(sampler.sample()[0], 400)

    def test_child_exit_between_enumeration_and_open(self):
        api = Fake(); api.tree.pop(3); original = api.open
        def race(pid):
            if pid == 2:
                api.dead.add(2)
                raise OSError("exited")
            return original(pid)
        api.open = race
        self.assertEqual(self.make(api).sample()[0], 100)

    def test_verified_child_exit_during_fresh_identity_query(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original = api.identity
        def race(handle):
            if handle == 2:
                api.dead.add(2)
                raise OSError("QueryFullProcessImageNameW after natural exit")
            return original(handle)
        api.identity = race
        events = []; sampler.event = events.append
        self.assertEqual(sampler.sample()[0], 400)
        events = [event for event in events if event["event"] == "verified-child-exited-during-identity-query"]
        self.assertEqual(events[0]["creationTime100ns"], 11)
        self.assertEqual(events[0]["exitTime100ns"], 102)
        self.assertEqual(events[0]["image"], api.identities[2][1])

    def test_known_child_natural_exit_during_reopen(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original = api.open
        def race(pid):
            if pid == 2:
                api.dead.add(2)
                raise OSError("naturally exited between enumeration and reopen")
            return original(pid)
        api.open = race
        self.assertEqual(sampler.sample()[0], 400)

    def test_leaf_exit_before_first_identity_has_lifetime_proof(self):
        api = Fake(); api.tree.pop(3); original = api.identity
        def race(handle):
            if handle == 2:
                api.dead.add(2)
                raise OSError("exited before image query")
            return original(handle)
        api.identity = race
        sampler = self.make(api); events = []; sampler.event = events.append
        self.assertEqual(sampler.sample()[0], 100)
        self.assertEqual(sampler.owned[2]["creationTime100ns"], 11)
        self.assertEqual(sampler.owned[2]["observedExitTime100ns"], 102)
        self.assertTrue(sampler.owned[2]["exitedBeforeIdentity"])

    def test_unidentified_exited_ancestor_with_live_descendant_rejected(self):
        api = Fake(); original = api.identity
        def race(handle):
            if handle == 2:
                api.dead.add(2)
                raise OSError("unidentified ancestor exited")
            return original(handle)
        api.identity = race
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "unverified ancestor has descendants"):
            self.make(api).sample()

    def test_live_child_identity_query_failure_rejected(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original = api.identity
        api.identity = lambda handle: (_ for _ in ()).throw(OSError("unreadable")) if handle == 2 else original(handle)
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "alive/unreadable descendant identity"):
            sampler.sample()

    def test_nonsignaled_child_exit_timestamp_is_not_exit_proof(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original_identity, original_times = api.identity, api.times
        api.identity = lambda handle: (_ for _ in ()).throw(OSError("unreadable")) if handle == 2 else original_identity(handle)
        api.times = lambda handle: (11, 102) if handle == 2 else original_times(handle)
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "alive/unreadable descendant identity"):
            sampler.sample()

    def test_signaled_child_missing_exit_timestamp_rejected(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original_identity, original_times = api.identity, api.times
        def race(handle):
            if handle == 2:
                api.dead.add(2)
                raise OSError("query failure")
            return original_identity(handle)
        api.identity = race
        api.times = lambda handle: (11, 0) if handle == 2 else original_times(handle)
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "signaled child lacks an exit timestamp"):
            sampler.sample()

    def test_exited_reused_pid_identity_failure_rejected(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original = api.identity
        def race(handle):
            if handle == 2:
                api.identities[2] = (999, pc11.canonical("compute/worker.exe"))
                api.dead.add(2)
                raise OSError("query failure after PID reuse")
            return original(handle)
        api.identity = race
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "PID reused during identity"):
            sampler.sample()

    def test_verified_child_exit_during_pinned_identity_query(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original = api.identity; calls = {2: 0}
        def race(handle):
            if handle == 2:
                calls[2] += 1
                if calls[2] == 2:  # fresh revalidation succeeded; retained query races exit
                    api.dead.add(2)
                    raise OSError("query after exit")
            return original(handle)
        api.identity = race
        self.assertEqual(sampler.sample()[0], 400)

    def test_live_pinned_identity_query_failure_rejected(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original = api.identity; calls = {2: 0}
        def race(handle):
            if handle == 2:
                calls[2] += 1
                if calls[2] == 2:
                    raise OSError("still live but unreadable")
            return original(handle)
        api.identity = race
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "alive/unreadable pinned identity"):
            sampler.sample()

    def test_unknown_descendant_rejected(self):
        api = Fake(); api.identities[2] = (11, pc11.canonical("unknown.exe"))
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "unknown"):
            self.make(api).sample()

    def test_ancestry_creation_rejected(self):
        api = Fake(); api.identities[2] = (9, pc11.canonical("compute/worker.exe"))
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "ancestry"):
            self.make(api).sample()

    def test_invalid_query_values(self):
        for value in (math.inf, math.nan, -1, 1.5, True, None):
            with self.subTest(value=value), self.assertRaises(pc11.InvalidMeasurement):
                pc11.checked_bytes(value)

    def test_verified_identity_failure_precedes_exit_signal(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        original = api.identity; waits = []; events = []; sampler.event = events.append
        api.identity = lambda h: (_ for _ in ()).throw(OSError(31, "image teardown")) if h == 2 else original(h)
        def wait(handle, timeout_ms):
            waits.append(timeout_ms); api.dead.add(handle); return True
        api.wait_exited = wait
        self.assertEqual(sampler.sample()[0], 400)
        self.assertEqual(len(waits), 1)
        self.assertTrue(0 < waits[0] <= pc11.EXIT_WAIT_BUDGET_MS)
        self.assertEqual(events[0]["errno"], 31)
        self.assertEqual(events[0]["stage"], "fresh")
        self.assertEqual(sampler.owned[2]["observedExitTime100ns"], 102)

    def test_unverified_live_child_never_waits(self):
        api = Fake(); original = api.identity; waits = []
        api.identity = lambda h: (_ for _ in ()).throw(OSError(5, "denied")) if h == 2 else original(h)
        api.wait_exited = lambda *args: waits.append(args)
        with self.assertRaisesRegex(pc11.InvalidMeasurement, "unreadable process identity"):
            self.make(api).sample()
        self.assertEqual(waits, [])

    def test_current_and_pinned_lifetimes_must_match(self):
        for current_times, pinned_times, pinned_signaled in [
                ((999, 102), (11, 102), True),
                ((11, 102), (999, 102), True),
                ((11, 102), (11, 103), True),
                ((11, 102), (11, 0), True),
                ((11, 102), (11, 102), False)]:
            with self.subTest(current=current_times, pinned=pinned_times, signaled=pinned_signaled):
                api = Fake(); sampler = self.make(api); sampler.sample()
                api.times = lambda h: current_times if h == 20 else pinned_times
                api.exited = lambda h: True if h == 20 else pinned_signaled
                with self.assertRaises(pc11.InvalidMeasurement):
                    sampler.confirmed_exit(2, sampler.owned[2], 20)

    def test_signal_requires_post_signal_timestamp_read(self):
        api = Fake(); sampler = self.make(api); sampler.sample(); signaled = [False]
        api.times = lambda h: (11, 102 if signaled[0] else 0)
        api.exited = lambda h: signaled[0]
        def wait(handle, timeout):
            signaled[0] = True; return True
        api.wait_exited = wait
        self.assertTrue(sampler.confirmed_exit(2, sampler.owned[2], 2))

    def test_multiple_exit_waits_share_budget_and_keep_50ms_schedule(self):
        api = Fake(); sampler = self.make(api); sampler.sample()
        clock = [0.0]; wait_total = [0.0]; stamps = []
        def wait(handle, timeout):
            advance = min(timeout / 1000, .003)
            clock[0] += advance; wait_total[0] += advance
            api.dead.add(handle); return True
        api.wait_exited = wait
        with patch.object(pc11.time, "monotonic", side_effect=lambda: clock[0]):
            for index in range(3):
                stamps.append(clock[0]); sampler.exit_wait_deadline = None
                api.dead.clear()
                self.assertTrue(sampler.confirmed_exit(2, sampler.owned[2], 2))
                self.assertTrue(sampler.confirmed_exit(3, sampler.owned[3], 3))
                self.assertLessEqual(wait_total[0], (index + 1) * .005 + 1e-12)
                clock[0] = (index + 1) * .05
        self.assertLessEqual(max(b-a for a,b in zip(stamps, stamps[1:])), .05)

    def test_preexisting_stop_is_unmeasured_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); stop = root / "stop"; stop.write_text("stop")
            with patch.object(pc11, "WindowsProcesses", return_value=Fake()), patch("builtins.print"):
                status = pc11.main(["--root-pid", "1", "--expected-root-image", str(root / "gui.exe"),
                                    "--output", str(root / "output"), "--stop-file", str(stop)])
            summary = json.loads((root / "output/summary.json").read_text())
            self.assertEqual(status, 1)
            self.assertEqual(summary["status"], "UNMEASURED")
            self.assertFalse(summary["withinMemoryLimit"])
            self.assertIsNone(summary["maximumWorkingSetBytes"])

    @unittest.skipUnless(os.name == "nt" and os.environ.get("PC11_SELF_PROBE") == "1", "opt-in Windows probe")
    def test_real_windows_self_pid(self):
        api = pc11.WindowsProcesses(); handle = api.open(os.getpid())
        try:
            created, image = api.identity(handle)
            self.assertGreater(created, 0)
            self.assertFalse(api.exited(handle))
            self.assertGreater(api.working_set(handle), 0)
            sampler = pc11.Sampler(api, os.getpid(), image, [], lambda event: None)
            try:
                self.assertGreater(sampler.sample()[0], 0)
            finally:
                sampler.close()
        finally:
            api.close(handle)


if __name__ == "__main__":
    unittest.main()
