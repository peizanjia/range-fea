# External excitation/controller Python SDK

`pure_provider.py` runs with `python pure_provider.py` on stdin/stdout or
`python pure_provider.py --tcp 6011` as a persistent localhost TCP server.
The workflow connection selects `transport: stdio` (Python executable plus
separate script argument), or `transport: tcp`, `host: 127.0.0.1`, `port: 6011`.
Pin algorithm `{ "id": "range-example-pure-provider", "version": "1" }`.
Include the script, `range_external_sdk.py`, imported modules and any weights in
the connection dependency list. Local artifacts are hashed; a TCP peer's claimed
algorithm version cannot establish its code identity and cannot certify resume.

Handshake advertises the exact physical ports. Input/output records contain
`id`, `value`, `unit`, `reference`; controller outputs are absolute `V`, excitation
outputs are `N` or `N m`. Counters are decimal strings. The host sends accepted
state and its SHA-256 in each evaluate. The callback returns outputs and a
candidate state; only a successful native FE transaction may accept the latter.
Repeated evaluation of the same state/input must yield the same result.

Range FEA generates wire IDs with `readout:`, `actuator:`, `state:`, and `force:`
prefixes. The `binding` field maps each wire ID to a native model output,
electrode, FE state coordinate, or force channel. This keeps IDs unique when an
electrode is both sensed and driven. Simulation time is finite and signed because
the initial FE boundary may precede zero when a model starts at zero.

The example provides two or more independent sinusoidal force/moment channels
and a velocity/PI controller. Excitation and controller state have separate
namespaces. This is a protocol example, not a tuned controller or hardware driver.
The host owns periods, zero-order hold, physical mappings, limits, logs and FE
rollback. `Test connection` only calls handshake, never evaluate.

`protocol.schema.json` is the machine-readable request/response schema. Runtime
checks also reject duplicate JSON keys, nonfinite numbers, wrong port sets or
units, stale state digests, identity/sequence mismatches, and invalid intervals.
Acceleration units follow the native catalog exactly: `m/s^2`, `rad/s^2`.

The runtime module exposes `--inspect connection.json` for offline file identity
inspection and `--test spec.json` for a handshake-only test. The latter accepts
`{"connection": {...}, "ports": [...]}` and optional run/digest/seed fields. Both
emit one JSON result on stdout and return nonzero on errors. Store the returned
`identity` as connection `expectedIdentity` to freeze file content; changed
executables, scripts or declared dependencies require a reload and new preview.

To write an algorithm, import `serve` from `range_external_sdk`; pass
`initialize(handshake) -> {role: state}` and
`evaluate(request) -> (output_records, candidate_state)`. Do not store mutable
algorithm state in globals. Diagnostics go to stderr. The SDK leaves connections
idle indefinitely between requests; the host bounds each active exchange and
can close/cancel it. `close` ends a session, not a user-owned TCP listener.

## C++ stdio example

Build and launch `range-external-io-cpp` as a stdio provider. The C++ example
uses Qt Core's JSON parser and implements the same versioned handshake and
evaluate envelopes. Its controller accepts velocity inputs in `m/s` and emits
absolute voltage in `V`; its excitation role emits deterministic force/moment
sines. Replace the example equations with your algorithm while keeping state in
the request and returning candidate state. The host accepts that candidate only
after the FE step succeeds.

## MATLAB TCP example

Add this directory to the MATLAB path and run `matlab_provider(6011)` (or
`matlab -batch "addpath('.../external_io'); matlab_provider(6011)"`). Configure
the connection as `tcp`, host `127.0.0.1`, port `6011`, algorithm ID
`range-example-matlab-provider`, version `1`. The example uses Java
`ServerSocket`, waits between FE requests without an idle timeout, and starts a
fresh explicit-state session for each accepted client. Closing a client leaves
the user's listener running.
