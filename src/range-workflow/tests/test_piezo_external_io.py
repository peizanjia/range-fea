"""Real process/socket protocol checks; these do not qualify native FE integration."""
import copy
import json
import io as streams
import os
from pathlib import Path
import socket
import sys
import subprocess
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import piezo_external_io as io

EXAMPLES = Path(__file__).resolve().parents[2] / "fea-solver" / "examples" / "external_io"
ALGORITHM = {"id": "range-example-pure-provider", "version": "1"}
PORTS = [
    {"id": "load", "direction": "output", "role": "excitation", "quantity": "force", "unit": "N", "reference": "increment"},
    {"id": "velocity", "direction": "input", "role": "controller", "quantity": "velocity", "unit": "m/s", "reference": "increment"},
    {"id": "voltage", "direction": "output", "role": "controller", "quantity": "voltage", "unit": "V", "reference": "absolute"},
]


def config(**fields):
    result = {"transport": "stdio", "executable": sys.executable,
              "args": ["-u", str(EXAMPLES / "pure_provider.py")], "algorithm": ALGORITHM,
              "dependencies": [str(EXAMPLES / "range_external_sdk.py")],
              "handshakeTimeout": 3., "requestTimeout": 1.}
    result.update(fields)
    return result


def client(configuration=None, **kwargs):
    return io.ExternalIOClient(configuration or config(), run_id="run-test", model_digest="a" * 64,
        config_digest="b" * 64, mapping_digest="c" * 64, ports=PORTS, **kwargs)


def sample(c, state=None, **kwargs):
    args = dict(step="9007199254740993", tick="1", time=.25, sample_period=.1,
        state=state if state is not None else {"integral": 0.},
        inputs=[{"id": "velocity", "value": 2., "unit": "m/s", "reference": "increment"}],
        previous_applied=[{"id": "voltage", "value": 0., "unit": "V", "reference": "absolute"}])
    args.update(kwargs)
    return c.evaluate("controller", **args)


class Peer:
    """One genuine local TCP peer with programmable wire corruption."""
    def __init__(self, mutate=None, delay=0, split=False):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.mutate = mutate
        self.delay = delay
        self.split = split
        self.closed = threading.Event()
        self.connection = None
        self.operations = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            self.connection, _ = self.listener.accept()
            with self.connection, self.connection.makefile("rb") as reader:
                while True:
                    line = reader.readline()
                    if not line:
                        return
                    request = json.loads(line)
                    self.operations.append(request["op"])
                    response = {k: request[k] for k in ("protocol", "op", "runId", "requestId")}
                    if request["op"] == "handshake":
                        response.update({k: request[k] for k in ("modelDigest", "configDigest", "mappingDigest")})
                        response.update(algorithm=ALGORITHM, ports=request["ports"],
                            capabilities={"explicitState": True}, initialState={"controller": {"integral": 0}, "excitation": {}})
                    elif request["op"] == "evaluate":
                        response.update({k: request[k] for k in ("step", "tick", "acceptedStateDigest")})
                        response.update(outputs=[{"id": "voltage", "value": -4., "unit": "V", "reference": "absolute"}],
                                        candidateState={"integral": 123})
                    payload = io.encode(response) + b"\r\n"
                    if self.mutate:
                        payload = self.mutate(request, response, payload)
                    if request["op"] == "evaluate" and self.delay:
                        self.closed.wait(self.delay)
                    if payload is not None:
                        if self.split:
                            for b in payload:
                                self.connection.sendall(bytes([b]))
                        else:
                            self.connection.sendall(payload)
                    if request["op"] == "close":
                        return
        except (OSError, ValueError):
            pass
        finally:
            self.closed.set()

    def config(self):
        return {"transport": "tcp", "host": "127.0.0.1", "port": self.port,
                "algorithm": ALGORITHM, "handshakeTimeout": 3, "requestTimeout": .2}

    def close(self):
        self.closed.set()
        if self.connection:
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.listener.close()
        self.thread.join(1)


class ExternalIOTest(unittest.TestCase):
    def wait_for_file(self, path, process, timeout=5):
        deadline = time.monotonic() + timeout
        while not path.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(.02)
        if not path.exists():
            if process.poll() is None:
                process.kill()
            stdout, stderr = process.communicate(timeout=3)
            self.fail(f"child did not become ready: stdout={stdout!r}, stderr={stderr!r}")

    def test_process_handshake_pure_candidates_both_roles_and_close(self):
        with client() as c:
            response = c.handshake()
            self.assertEqual(response["initialState"]["controller"], {"integral": 0})
            self.assertEqual(c._serial, 1)  # Connection test never evaluates.
            accepted = copy.deepcopy(response["initialState"]["controller"])
            first, retry = sample(c, accepted), sample(c, accepted)
            self.assertEqual(first["candidateState"], retry["candidateState"])
            self.assertEqual(first["outputs"], retry["outputs"])
            self.assertEqual(accepted, {"integral": 0})
            self.assertAlmostEqual(first["outputs"][0]["value"], -4.02)
            ex = c.evaluate("excitation", step="0", tick="0", time=0., sample_period=.1, state={"samples": "0"})
            self.assertEqual(ex["candidateState"], {"samples": "1"})
            process = c._process
        self.assertIsNotNone(process.poll())

    def test_signed_initial_simulation_time_is_supported(self):
        with client() as c:
            c.handshake()
            response = sample(c, time=-.01, valid_until=0.)
            self.assertEqual(response["step"], "9007199254740993")
            self.assertTrue(all(item["unit"] == "V" for item in response["outputs"]))

    def test_cpp_reference_provider_handshake_and_both_roles(self):
        executable = os.environ.get("RANGE_EXTERNAL_CPP_PROVIDER")
        if not executable or not Path(executable).is_file():
            self.skipTest("C++ reference provider was not built")
        cfg = config(executable=str(Path(executable).resolve()), args=[], dependencies=[],
                     algorithm={"id": "range-example-cpp-provider", "version": "1"},
                     handshakeTimeout=5., requestTimeout=2.)
        with client(cfg) as c:
            initial = c.handshake()["initialState"]
            force = c.evaluate("excitation", step="0", tick="0", time=.125,
                sample_period=.1, state=initial["excitation"], inputs=[])
            self.assertAlmostEqual(force["outputs"][0]["value"], .05 / (2 ** .5), places=12)
            controller = c.evaluate("controller", step="1", tick="1", time=.1,
                sample_period=.1, state=initial["controller"],
                inputs=[{"id":"velocity","value":2.,"unit":"m/s","reference":"increment"}],
                previous_applied=[{"id":"voltage","value":0.,"unit":"V","reference":"absolute"}])
            self.assertAlmostEqual(controller["outputs"][0]["value"], -4.02)
            self.assertAlmostEqual(controller["candidateState"]["integral"], .2)

    def test_tcp_split_crlf_long_idle_and_external_listener_ownership(self):
        peer = Peer(split=True)
        try:
            with client(peer.config()) as c:
                c.handshake()
                time.sleep(.25)  # More than active request timeout; idle is valid.
                self.assertEqual(sample(c)["outputs"][0]["value"], -4.)
            self.assertEqual(peer.operations, ["handshake", "evaluate", "close"])
            self.assertNotEqual(peer.listener.fileno(), -1)
        finally:
            peer.close()

    def test_malformed_wire_and_identity_responses_leave_state_unchanged(self):
        def fault(kind):
            def mutate(request, response, payload):
                if request["op"] != "evaluate":
                    return payload
                if kind == "bad-json": return b"{broken}\n"
                if kind == "bom": return b"\xef\xbb\xbf" + payload
                if kind == "duplicate-json": return b'{"op":"evaluate","op":"evaluate"}\n'
                if kind == "nan": return payload.replace(b"-4.0", b"NaN")
                if kind == "overflow": return payload.replace(b"-4.0", b"1e999")
                if kind == "long": return b"x" * 4097
                if kind == "coalesced": return payload + payload
                if kind == "missing": response["outputs"] = []
                elif kind == "duplicate": response["outputs"] *= 2
                elif kind == "unit": response["outputs"][0]["unit"] = "N"
                elif kind == "reference": response["outputs"][0]["reference"] = "increment"
                elif kind in ("runId", "requestId", "step", "tick", "acceptedStateDigest", "op"):
                    response[kind] = "wrong"
                return io.encode(response) + b"\n"
            return mutate
        for kind in ("bad-json", "bom", "duplicate-json", "nan", "overflow", "long", "coalesced", "missing", "duplicate", "unit", "reference",
                     "runId", "requestId", "step", "tick", "acceptedStateDigest", "op"):
            with self.subTest(kind=kind):
                peer = Peer(fault(kind))
                cfg = peer.config()
                cfg["maxLineBytes"] = 4096
                accepted = {"integral": 0}
                try:
                    with client(cfg) as c:
                        c.handshake()
                        with self.assertRaises(io.ExternalIOError): sample(c, accepted)
                        self.assertTrue(c._closed.is_set())
                    self.assertEqual(accepted, {"integral": 0})
                finally:
                    peer.close()

    def test_timeout_total_deadline_and_cancellation(self):
        for mode in ("timeout", "deadline", "cancel", "close"):
            with self.subTest(mode=mode):
                peer = Peer(delay=2)
                event = threading.Event()
                c = client(peer.config(), cancel_event=event)
                timer = None
                try:
                    c.handshake()
                    if mode in ("cancel", "close"):
                        timer = threading.Timer(.05, event.set if mode == "cancel" else c.close)
                        timer.start()
                    start = time.monotonic()
                    with self.assertRaises(io.ExternalIOError):
                        sample(c, deadline=start + .04 if mode == "deadline" else None)
                    self.assertLess(time.monotonic() - start, .8)
                finally:
                    c.close()
                    if timer: timer.join()
                    peer.close()

    def test_configuration_ports_and_dependency_identity(self):
        for changes in ({"transport": "shell"}, {"requestTimeout": 60}, {"maxLineBytes": True},
                        {"args": "not-a-list"}, {"algorithm": {"id": "x"}}):
            with self.assertRaises(io.ExternalIOError): io.normalize_config(config(**changes))
        for host in ("192.168.1.1", "example.org", "127.0.0.1.example.org"):
            with self.assertRaises(io.ExternalIOError):
                io.normalize_config({"transport": "tcp", "host": host, "port": 42, "algorithm": ALGORITHM})
        for key, value in (("unit", "mV"), ("reference", "relative"), ("direction", "out")):
            bad = copy.deepcopy(PORTS)
            bad[2][key] = value
            with self.assertRaises(io.ExternalIOError): io.validate_ports(bad)
        with self.assertRaises(io.ExternalIOError): io.validate_ports(PORTS + [PORTS[0]])
        with tempfile.TemporaryDirectory() as directory:
            dependency = Path(directory) / "weights.dat"
            dependency.write_bytes(b"one")
            cfg = config(dependencies=[str(dependency)])
            first = io.fingerprint(cfg)
            c = client(cfg)
            dependency.write_bytes(b"two")
            self.assertNotEqual(io.fingerprint(cfg), first)
            with self.assertRaisesRegex(io.ExternalIOError, "identity changed"): c.handshake()
            self.assertIsNone(c._process)
        identity = io.inspect_config(config())
        self.assertEqual(identity["identityTrust"], "local-files")
        self.assertIn(str((EXAMPLES / "pure_provider.py").resolve()), [f["path"] for f in identity["files"]])

    def test_invalid_local_signals_never_send_or_mutate_state(self):
        with client() as c:
            c.handshake()
            for overrides in ({"step": 3}, {"inputs": []}, {"previous_applied": []}, {"time": float("inf")},
                              {"sample_period": 0}, {"state": {"a": float("nan")}}):
                with self.assertRaises(io.ExternalIOError): sample(c, **overrides)
            self.assertEqual(c._serial, 1)
            self.assertEqual(sample(c)["candidateState"], {"integral": .2})

    def test_process_exit_stderr_and_bounded_timeout_cleanup(self):
        for code in ("import sys;sys.stderr.write('diagnostic');sys.exit(3)",
                     "import time;time.sleep(10)"):
            with self.subTest(code=code):
                cfg = config(args=["-u", "-c", code], handshakeTimeout=.15)
                c = client(cfg)
                start = time.monotonic()
                with self.assertRaises(io.ExternalIOError): c.handshake()
                self.assertLess(time.monotonic() - start, 1.)
                self.assertIsNotNone(c._process.poll())
                c.close()

    def test_expected_identity_pins_files_and_runtime_detects_mid_session_change(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "weights"
            path.write_bytes(b"first")
            cfg = config(dependencies=[str(path)])
            cfg["expectedIdentity"] = io.inspect_config(cfg)
            with client(cfg) as c:
                c.handshake()
                path.write_bytes(b"second")
                with self.assertRaisesRegex(io.ExternalIOError, "identity changed"):
                    sample(c)
            with self.assertRaisesRegex(io.ExternalIOError, "reload required"):
                io.normalize_config(cfg)

    def test_cli_offline_inspection_and_handshake_only(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "connection.json"
            path.write_bytes(io.encode(config()))
            module = Path(io.__file__)
            result = subprocess.run([sys.executable, str(module), "--inspect", str(path)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(io.decode(result.stdout)["identity"], io.inspect_config(config()))
            peer = Peer()
            try:
                path.write_bytes(io.encode({"connection": peer.config(), "ports": PORTS}))
                result = subprocess.run([sys.executable, str(module), "--test", str(path)], capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertTrue(io.decode(result.stdout)["ok"])
                self.assertEqual(peer.operations, ["handshake", "close"])
            finally:
                peer.close()

    def test_sdk_rejects_unknown_operations_duplicate_fields_and_bad_input(self):
        for line in (b'{"protocol":"range-external-io/1","op":"commit","runId":"r","requestId":"1"}\n',
                     b'{"op":"handshake","op":"handshake"}\n', b"\xef\xbb\xbf{}\n", b"[]\n"):
            with self.assertRaises(io.ExternalIOError):
                io.serve(streams.BytesIO(line), streams.BytesIO(), algorithm=ALGORITHM,
                         initialize=lambda r: {}, evaluate=lambda r: ([], {}))

    def test_machine_schema_matches_real_handshake_and_evaluate(self):
        try:
            from jsonschema import Draft202012Validator
        except ImportError:
            self.skipTest("optional jsonschema verifier unavailable")
        schema = json.loads((EXAMPLES / "protocol.schema.json").read_text(encoding="utf-8"))
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema)
        with client() as c:
            validator.validate(c.handshake())
            validator.validate(sample(c))
        invalid = copy.deepcopy(PORTS[0])
        invalid["unit"] = "V"
        self.assertFalse(Draft202012Validator(schema["$defs"]["port"]).is_valid(invalid))

    @unittest.skipUnless(os.name == "nt", "Windows atomic Job Object containment")
    def test_abrupt_backend_kill_terminates_evaluating_provider_and_descendant(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.TerminateProcess.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            provider, owner = directory / "provider.py", directory / "owner.py"
            pids, ready, spec = directory / "pids.json", directory / "evaluating", directory / "connection.json"
            provider.write_text(
                "import json,os,subprocess,sys,time\nfrom pathlib import Path\n"
                f"sys.path.insert(0, {str(Path(io.__file__).resolve().parents[1])!r})\n"
                "from range_workflow.piezo_external_io import serve\n"
                "def initialize(request):\n"
                " child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])\n"
                f" Path({str(pids)!r}).write_text(json.dumps({{'provider':os.getpid(),'descendant':child.pid}}))\n"
                " return {role:{} for role in {p['role'] for p in request['ports']}}\n"
                "def evaluate(request):\n"
                f" Path({str(ready)!r}).write_text('evaluate started')\n"
                " time.sleep(60)\n return [],{}\n"
                f"serve(sys.stdin.buffer,sys.stdout.buffer,algorithm={ALGORITHM!r},initialize=initialize,evaluate=evaluate)\n",
                encoding="utf-8")
            spec.write_bytes(io.encode(config(args=["-u", str(provider)], requestTimeout=55)))
            owner.write_text(
                "import json,sys\n"
                f"sys.path.insert(0,{str(Path(__file__).resolve().parent)!r})\n"
                "from test_piezo_external_io import client,sample\n"
                "c=client(json.load(open(sys.argv[1],encoding='utf-8')))\n"
                "c.handshake()\nsample(c,state={})\n", encoding="utf-8")
            process = subprocess.Popen([sys.executable, "-u", str(owner), str(spec)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            handles = []
            try:
                self.wait_for_file(ready, process)
                for name, pid in json.loads(pids.read_text()).items():
                    handle = kernel.OpenProcess(0x100001, False, pid)  # SYNCHRONIZE | PROCESS_TERMINATE
                    self.assertTrue(handle, (name, pid, ctypes.get_last_error()))
                    handles.append((name, handle))
                    self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258, name)
                process.kill()  # No finally, close callback or protocol close can run.
                process.wait(timeout=3)
                for name, handle in handles:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 2000), 0, f"orphaned {name}")
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=3)
                for _, handle in handles:
                    if kernel.WaitForSingleObject(handle, 0) != 0:
                        kernel.TerminateProcess(handle, 1)
                        kernel.WaitForSingleObject(handle, 2000)
                    kernel.CloseHandle(handle)

    def test_abrupt_backend_kill_preserves_user_owned_tcp_listener(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            # The listener belongs to this test/user, not the backend process.
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
            listener.close()
            server = subprocess.Popen([sys.executable, "-u", str(EXAMPLES / "pure_provider.py"), "--tcp", str(port)],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            owner = None
            try:
                self.assertIn(b"Listening", server.stderr.readline())
                configuration = {"transport": "tcp", "host": "127.0.0.1", "port": port,
                                 "algorithm": ALGORITHM}
                spec, ready, script = directory / "spec.json", directory / "ready", directory / "owner.py"
                spec.write_bytes(io.encode(configuration))
                script.write_text(
                    "import json,sys,time\nfrom pathlib import Path\n"
                    f"sys.path.insert(0,{str(Path(__file__).resolve().parent)!r})\n"
                    "from test_piezo_external_io import client\n"
                    "c=client(json.load(open(sys.argv[1],encoding='utf-8')))\nc.handshake()\n"
                    f"Path({str(ready)!r}).write_text('connected')\ntime.sleep(60)\n", encoding="utf-8")
                owner = subprocess.Popen([sys.executable, "-u", str(script), str(spec)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.wait_for_file(ready, owner)
                owner.kill()
                owner.wait(timeout=3)
                self.assertIsNone(server.poll())
                # A fresh connection proves the same user-owned server remains usable.
                with client(configuration) as c:
                    c.handshake()
                    self.assertAlmostEqual(sample(c)["outputs"][0]["value"], -4.02)
            finally:
                if owner:
                    if owner.poll() is None:
                        owner.kill()
                    owner.communicate(timeout=3)
                if server.poll() is None:
                    server.kill()
                server.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main()
