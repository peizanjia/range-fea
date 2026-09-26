#!/usr/bin/env python3
"""Bounded NDJSON stdin/stdout to TCP bridge for external control processors.

The endpoint must implement the solver's pure-step protocol. No retries or
reconnections are made: loss, timeout, or malformed response fails the FE step.
This is a simulation/HIL transport, not a hard real-time hardware driver.
"""
from __future__ import annotations

import argparse
import json
import math
import socket
import sys
import time

MAX_LINE = 1024 * 1024


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('duplicate JSON key')
        value[key] = item
    return value


def decode(line: bytes) -> dict:
    def reject_constant(value):
        raise ValueError('non-finite JSON constant')
    value = json.loads(line.decode('utf-8'), object_pairs_hook=unique_object,
                       parse_constant=reject_constant)
    def validate(item, depth=0):
        if depth > 32:
            raise ValueError('JSON nesting exceeds 32')
        if isinstance(item, float) and not math.isfinite(item):
            raise ValueError('non-finite number')
        if isinstance(item, dict):
            for child in item.values(): validate(child, depth + 1)
        elif isinstance(item, list):
            for child in item: validate(child, depth + 1)
    validate(value)
    if not isinstance(value, dict):
        raise ValueError('JSON object required')
    return value


def exchange(connection, request_line: bytes, timeout: float, expected_endpoint: str | None = None) -> bytes:
    if len(request_line) > MAX_LINE or not request_line.endswith(b'\n'):
        raise ValueError('request line exceeds limit or lacks newline')
    request = decode(request_line)
    if (type(request.get('protocolVersion')) is not int or
            request['protocolVersion'] not in (1, 2) or
            request.get('op') not in ('handshake', 'compute') or
            not isinstance(request.get('requestId'), str) or not request['requestId']):
        raise ValueError('invalid request envelope')
    deadline = time.monotonic() + timeout
    connection.settimeout(timeout)
    connection.sendall(request_line)
    received = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('TCP response deadline exceeded')
        connection.settimeout(remaining)
        chunk = connection.recv(min(65536, MAX_LINE + 1 - len(received)))
        if not chunk:
            raise ConnectionError('TCP endpoint closed before response')
        received.extend(chunk)
        if len(received) > MAX_LINE:
            raise ValueError('response line exceeds limit')
        if b'\n' in received:
            line, tail = bytes(received).split(b'\n', 1)
            if tail:
                raise ValueError('unsolicited extra TCP response')
            response = decode(line)
            for field in ('protocolVersion', 'requestId', 'op'):
                if type(response.get(field)) is not type(request[field]) or response[field] != request[field]:
                    raise ValueError('response envelope mismatch: ' + field)
            if request['op'] == 'compute' and response.get('stepId') != request.get('stepId'):
                raise ValueError('response step mismatch')
            if request['op'] == 'handshake' and expected_endpoint is not None:
                if response.get('endpointSha256') != expected_endpoint:
                    raise ValueError('endpoint artifact/configuration digest mismatch')
            return line + b'\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', required=True, type=int)
    parser.add_argument('--timeout', type=float, default=5.0,
                        help='connection and each complete request/response deadline, seconds')
    parser.add_argument('--endpoint-sha256', help='pin endpoint implementation/config digest advertised at handshake')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not math.isfinite(args.timeout) or not 0 < args.timeout <= 120:
        parser.error('port must be 1..65535 and timeout in (0,120] seconds')
    if args.endpoint_sha256 is not None and (len(args.endpoint_sha256) != 64 or
            any(c not in '0123456789abcdef' for c in args.endpoint_sha256)):
        parser.error('endpoint-sha256 must be a lowercase SHA256 hex digest')
    try:
        with socket.create_connection((args.host, args.port), timeout=args.timeout) as connection:
            while True:
                line = sys.stdin.buffer.readline(MAX_LINE + 1)
                if not line:
                    return 0
                response = exchange(connection, line, args.timeout, args.endpoint_sha256)
                sys.stdout.buffer.write(response)
                sys.stdout.buffer.flush()
    except (OSError, ValueError, RecursionError) as error:
        print('TCP controller bridge failed: ' + str(error), file=sys.stderr, flush=True)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
