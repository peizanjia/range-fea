#!/usr/bin/env python3
"""Two-channel sine excitation and explicit-state PI/velocity controller.

Run via stdin/stdout (default), or --tcp PORT for a persistent local listener.
The TCP listener survives client close; no idle receive timeout is imposed.
Output IDs/units/references are taken from the handshake's physical mappings.
"""
import argparse
import math
import socket
import sys

from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from range_external_sdk import serve

ALGORITHM = {"id": "range-example-pure-provider", "version": "1"}


def session(reader, writer):
    ports = []

    def initialize(request):
        ports[:] = request["ports"]
        return {role: {"integral": 0.} if role == "controller" else {"samples": "0"}
                for role in {p["role"] for p in ports}}

    def evaluate(request):
        outputs = [p for p in ports if p["role"] == request["role"] and p["direction"] == "output"]
        state = request["state"]
        if request["role"] == "excitation":
            # Distinct phase/frequency per channel; deterministic at requested FE time.
            values = [(index + 1) * math.sin(2 * math.pi * (index + 1) * request["time"])
                      for index in range(len(outputs))]
            candidate = {"samples": str(int(state["samples"]) + 1)}
        else:
            measurement = sum(p["value"] for p in request["inputs"])
            integral = state["integral"] + request["samplePeriod"] * measurement
            # Requested absolute voltage. Native FE adapters own all limitations.
            values = [-2. * measurement - .1 * integral] * len(outputs)
            candidate = {"integral": integral}
        return ([{"id": port["id"], "unit": port["unit"], "reference": port["reference"], "value": value}
                 for port, value in zip(outputs, values)], candidate)

    serve(reader, writer, algorithm=ALGORITHM, initialize=initialize, evaluate=evaluate)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tcp", type=int, metavar="PORT")
    args = parser.parse_args()
    if args.tcp is None:
        session(sys.stdin.buffer, sys.stdout.buffer)
        return
    if not 1 <= args.tcp <= 65535:
        parser.error("port must be 1..65535")
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", args.tcp))
        listener.listen(4)
        print(f"Listening on 127.0.0.1:{args.tcp}", file=sys.stderr, flush=True)
        while True:
            connection, _ = listener.accept()
            with connection, connection.makefile("rb") as reader, connection.makefile("wb") as writer:
                try:
                    session(reader, writer)
                except (ValueError, OSError) as exc:
                    print(str(exc), file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
