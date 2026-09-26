"""Actual socket and child-process checks; no board/hard-real-time claim."""
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'examples' / 'control_tcp_bridge.py'
spec = importlib.util.spec_from_file_location('bridge', SCRIPT)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


def request(op='compute', request_id='one'):
    return {'protocolVersion': 1, 'op': op, 'requestId': request_id,
            'stepId': '7', 'acceptedControllerState': {'count': 3}}


class TcpBridgeTests(unittest.TestCase):
    def exchange(self, payload, timeout=1, envelope=None, expected_endpoint=None):
        client, server = socket.socketpair()
        failures = []
        def respond():
            try:
                with server:
                    server.recv(10000)
                    if callable(payload): payload(server)
                    else: server.sendall(payload)
            except OSError:
                pass  # Client deliberately closes on rejected/late response.
            except Exception as error:
                failures.append(error)
        thread = threading.Thread(target=respond)
        thread.start()
        try:
            with client:
                return bridge.exchange(client, (json.dumps(request() if envelope is None else envelope)+'\n').encode(), timeout, expected_endpoint)
        finally:
            thread.join(2)
            self.assertFalse(thread.is_alive())
            self.assertFalse(failures)

    def test_fragmented_response(self):
        raw = (json.dumps(request())+'\n').encode()
        def send(server):
            for chunk in (raw[:4], raw[4:10], raw[10:]):
                server.sendall(chunk)
        self.assertEqual(json.loads(self.exchange(send)), request())

    def test_wrong_id_and_step_rejected(self):
        for field, value in [('requestId', 'old'), ('stepId', '6'), ('protocolVersion', True)]:
            with self.subTest(field=field):
                bad = request(); bad[field] = value
                with self.assertRaises(ValueError): self.exchange((json.dumps(bad)+'\n').encode())

    def test_pinned_endpoint_configuration(self):
        envelope=request('handshake')
        reply={**envelope,'endpointSha256':'a'*64}
        raw=(json.dumps(reply)+'\n').encode()
        self.assertEqual(json.loads(self.exchange(raw,envelope=envelope,expected_endpoint='a'*64)),reply)
        with self.assertRaisesRegex(ValueError,'digest mismatch'):
            self.exchange(raw,envelope=envelope,expected_endpoint='b'*64)

    def test_truncated_duplicate_nonfinite_and_oversize_rejected(self):
        for payload in [b'{', b'{"x":1,"x":2}\n', b'{"x":NaN}\n',
                        b'{"x":1e999}\n', b'x'*(bridge.MAX_LINE+1),
                        (json.dumps(request())+'\n{}\n').encode()]:
            with self.subTest(length=len(payload)):
                with self.assertRaises((ValueError, ConnectionError)): self.exchange(payload)

    def test_deadline_covers_whole_response(self):
        def trickle(server):
            for _ in range(30):
                server.sendall(b' '); time.sleep(.02)
        started = time.monotonic()
        with self.assertRaises(TimeoutError): self.exchange(trickle, .08)
        self.assertLess(time.monotonic()-started, 1)

    def test_process_replay_and_clean_eof(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(5)
            errors = []
            def endpoint():
                try:
                    connection, _ = listener.accept()
                    with connection, connection.makefile('rwb') as stream:
                        for _ in range(3):
                            item = json.loads(stream.readline())
                            item['nextControllerState'] = {'count': item['acceptedControllerState']['count']+1}
                            stream.write((json.dumps(item)+'\n').encode()); stream.flush()
                        self.assertEqual(stream.read(), b'')
                except Exception as error: errors.append(error)
            thread = threading.Thread(target=endpoint); thread.start()
            raw = json.dumps(request())+'\n'
            process = subprocess.run([sys.executable, str(SCRIPT), '--port', str(listener.getsockname()[1])],
                                     input=raw*3, text=True, capture_output=True, timeout=10)
            thread.join(5)
            self.assertFalse(thread.is_alive()); self.assertFalse(errors, errors)
            self.assertEqual(process.returncode, 0, process.stderr)
            replies = [json.loads(line) for line in process.stdout.splitlines()]
            self.assertEqual(len(replies), 3)
            self.assertTrue(all(item == replies[0] for item in replies))
            self.assertEqual(replies[0]['nextControllerState']['count'], 4)


if __name__ == '__main__': unittest.main()
