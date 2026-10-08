"""Read-only projection of native accepted-control event journals."""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import stat
import threading
import uuid

from .service import WorkflowError

SOURCE = "native-accepted-control"
MAX_JOURNAL_BYTES = 32 * 1024 * 1024
MAX_READ_BYTES_PER_QUERY = 1024 * 1024
MAX_EVENT_BYTES = 256 * 1024
MAX_EVENTS = 65_536
MAX_SAMPLES = 5_000
MAX_CHANNELS = 4_096
MAX_CACHED_SAMPLE_BYTES = 16 * 1024 * 1024
MAX_CACHED_JOURNALS = 8
MAX_DIRECTORY_ENTRIES = 1_024
MAX_TRAJECTORY_DIRECTORY_ENTRIES = 8_192  # Up to 5,000 actual numbered MODEL frames plus bounded journals/metadata.
MAX_EXACT_INTEGER = (1 << 53) - 1
_JOURNAL_NAME = re.compile(
    r"^(active|passive)\.r(?:a|b)m\.([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\.events\.jsonl$"
)
_CHANNEL_ID = re.compile(r"^[^\s\x00-\x1f\x7f\"]{1,128}$")


@dataclass
class _Journal:
    path: Path
    run_id: str
    device: int
    inode: int
    offset: int = 0
    pending: bytes = b""
    next_event_sequence: int = 1
    event_count: int = 0
    event_ids: set[str] = field(default_factory=set)
    samples: list[dict] = field(default_factory=list)
    sample_bytes: int = 0
    finished: bool = False
    failure: tuple[str, str] | None = None


class TelemetryReader:
    """Incrementally parse journals while keeping reads and caches bounded."""

    def __init__(self, workspace: Path):
        self.workspace = Path(workspace).resolve()
        self._lock = threading.RLock()
        self._journals: OrderedDict[tuple[str, str], _Journal] = OrderedDict()
        self._cached_sample_bytes = 0

    def query(self, job_id: str, job: dict, mode: str, after_seq: int, limit: int,
              scheduled_mode: str | None) -> dict:
        state = job.get("state", "unknown")
        base = {"jobId": job_id, "mode": mode, "runId": None, "state": state,
                "source": SOURCE, "samples": [], "nextSeq": after_seq,
                "hasMore": False, "availability": "waiting"}

        if job.get("command") not in ("workflow.run", "workflow.resume"):
            base.update(availability="unavailable",
                        message="This job does not produce native control telemetry.")
            return base
        if scheduled_mode not in ("active", "passive", "both"):
            base.update(availability="unavailable",
                        message="The requested trajectory mode is unavailable for this job.")
            return base
        if scheduled_mode != "both" and scheduled_mode != mode:
            base.update(availability="unavailable",
                        message="The requested trajectory mode was not part of this workflow.")
            return base

        directory, present = self._job_directory(job_id)
        if not present:
            self._set_missing_availability(base, state)
            return base

        journal_path, run_id = self._find_journal(directory, mode)
        if journal_path is None:
            self._set_missing_availability(base, state)
            return base
        base["runId"] = run_id

        with self._lock:
            key = (job_id, mode)
            journal = self._journals.get(key)
            if journal is None:
                self._evict_for_new_journal()
                info = journal_path.stat()
                journal = _Journal(journal_path, run_id, info.st_dev, info.st_ino)
                self._journals[key] = journal
            elif journal.path != journal_path or journal.run_id != run_id:
                raise WorkflowError("TELEMETRY_CHANGED", "The solver journal changed during this job")
            else:
                self._journals.move_to_end(key)

            if journal.failure:
                raise WorkflowError(*journal.failure)
            try:
                self._read_available(journal)
            except WorkflowError as error:
                if error.code in {"TELEMETRY_CORRUPT", "TELEMETRY_LIMIT", "TELEMETRY_CHANGED"}:
                    journal.failure = (error.code, error.message)
                raise
            selected = [sample for sample in journal.samples if sample["seq"] > after_seq]
            page = selected[:limit]
            base["samples"] = [dict(sample) for sample in page]
            if page:
                base["nextSeq"] = page[-1]["seq"]
            try:
                size = journal_path.stat().st_size
            except OSError:
                raise WorkflowError("TELEMETRY_PATH", "The solver event journal is unavailable") from None
            base["hasMore"] = len(selected) > limit or journal.offset < size

            if mode == "passive" and journal.event_count:
                base.update(availability="unavailable",
                            message="Passive solves do not emit accepted control telemetry.")
            elif journal.samples:
                base["availability"] = "available"
            elif journal.finished or state in {"succeeded", "failed", "cancelled", "interrupted"}:
                base.update(availability="unavailable",
                            message="The solver run ended without accepted control samples.")
            elif journal.event_count:
                base["availability"] = "waiting"
                base["message"] = "Waiting for the first accepted control step."
        return base

    @staticmethod
    def _set_missing_availability(result: dict, state: str):
        if state in {"succeeded", "failed", "cancelled", "interrupted"}:
            result.update(availability="unavailable",
                          message="The job ended without creating a native event journal.")
        else:
            result["message"] = "The solver event journal has not been created yet."

    def _job_directory(self, job_id: str) -> tuple[Path, bool]:
        try:
            if str(uuid.UUID(job_id)) != job_id:
                raise ValueError
        except (ValueError, AttributeError):
            raise WorkflowError("INVALID_PATH", "Job directory is unavailable") from None

        jobs = self.workspace / "jobs"
        directory = jobs / job_id
        for path in (jobs, directory):
            try:
                info = path.lstat()
            except FileNotFoundError:
                return directory, False
            except OSError:
                raise WorkflowError("TELEMETRY_PATH", "Job directory is unavailable") from None
            if path.is_symlink() or not path.resolve().is_relative_to(self.workspace):
                raise WorkflowError("TELEMETRY_PATH", "Job directory escapes the workflow workspace")
            if path == jobs and not path.is_dir():
                raise WorkflowError("TELEMETRY_PATH", "Job directory is unavailable")
            if path == directory and not path.is_dir():
                raise WorkflowError("TELEMETRY_PATH", "Job directory is unavailable")
        return directory, True

    def _find_journal(self, directory: Path, mode: str) -> tuple[Path | None, str | None]:
        candidates: list[tuple[Path, str]] = []
        try:
            locations = [directory]
            variant_directory = directory / mode
            try:
                info = variant_directory.lstat()
            except FileNotFoundError:
                info = None
            if info is not None:
                if (variant_directory.is_symlink() or
                        getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT or
                        not stat.S_ISDIR(info.st_mode) or
                        not variant_directory.resolve(strict=True).is_relative_to(directory.resolve())):
                    raise WorkflowError("TELEMETRY_PATH", "Trajectory directory must be a regular managed directory")
                locations.append(variant_directory)
            for location in locations:
                limit = MAX_DIRECTORY_ENTRIES if location == directory else MAX_TRAJECTORY_DIRECTORY_ENTRIES
                with os.scandir(location) as entries:
                    for index, entry in enumerate(entries):
                        if index >= limit:
                            raise WorkflowError("TELEMETRY_LIMIT", "Job directory contains too many entries")
                        match = _JOURNAL_NAME.fullmatch(entry.name)
                        if match and match.group(1) == mode:
                            candidates.append((Path(entry.path), match.group(2)))
                            if len(candidates) > 1:
                                raise WorkflowError("TELEMETRY_AMBIGUOUS", "Multiple native event journals match this trajectory")
        except WorkflowError:
            raise
        except OSError:
            raise WorkflowError("TELEMETRY_PATH", "Job directory is unavailable") from None
        if not candidates:
            return None, None
        path, run_id = candidates[0]
        if path.is_symlink():
            raise WorkflowError("TELEMETRY_PATH", "Symbolic links are not accepted as solver journals")
        try:
            resolved = path.resolve(strict=True)
        except OSError:
            raise WorkflowError("TELEMETRY_PATH", "Solver journal is unavailable") from None
        if not resolved.is_relative_to(directory.resolve()) or not path.is_file():
            raise WorkflowError("TELEMETRY_PATH", "Solver journal escapes the job directory")
        return path, run_id

    def _evict_for_new_journal(self):
        while len(self._journals) >= MAX_CACHED_JOURNALS:
            _, oldest = self._journals.popitem(last=False)
            self._cached_sample_bytes -= oldest.sample_bytes

    def _read_available(self, journal: _Journal):
        try:
            current = journal.path.stat()
        except OSError:
            raise WorkflowError("TELEMETRY_PATH", "The solver event journal is unavailable") from None
        if journal.path.is_symlink() or (current.st_dev, current.st_ino) != (journal.device, journal.inode):
            raise WorkflowError("TELEMETRY_CHANGED", "The solver journal changed during this job")
        if current.st_size < journal.offset:
            raise WorkflowError("TELEMETRY_CHANGED", "The solver journal was truncated during this job")
        if current.st_size > MAX_JOURNAL_BYTES:
            raise WorkflowError("TELEMETRY_LIMIT", "Solver event journal exceeds the size limit")
        amount = min(current.st_size - journal.offset, MAX_READ_BYTES_PER_QUERY)
        if amount <= 0:
            if len(journal.pending) > MAX_EVENT_BYTES:
                raise WorkflowError("TELEMETRY_LIMIT", "Solver event line exceeds the size limit")
            return
        try:
            with journal.path.open("rb") as stream:
                opened = os.fstat(stream.fileno())
                if (journal.path.is_symlink() or
                        (opened.st_dev, opened.st_ino) != (journal.device, journal.inode) or
                        not journal.path.resolve().is_relative_to(self.workspace)):
                    raise WorkflowError("TELEMETRY_CHANGED", "The solver journal changed while it was opened")
                stream.seek(journal.offset)
                chunk = stream.read(amount)
        except WorkflowError:
            raise
        except OSError:
            raise WorkflowError("TELEMETRY_PATH", "The solver event journal cannot be read") from None
        if len(chunk) != amount:
            raise WorkflowError("TELEMETRY_CHANGED", "The solver journal changed while it was read")
        journal.offset += len(chunk)
        data = journal.pending + chunk
        start = 0
        while True:
            newline = data.find(b"\n", start)
            if newline < 0:
                break
            line = data[start:newline]
            if line.endswith(b"\r"):
                line = line[:-1]
            if len(line) > MAX_EVENT_BYTES:
                raise WorkflowError("TELEMETRY_LIMIT", "Solver event line exceeds the size limit")
            self._consume_event(journal, line)
            start = newline + 1
        journal.pending = data[start:]
        if len(journal.pending) > MAX_EVENT_BYTES:
            raise WorkflowError("TELEMETRY_LIMIT", "Solver event line exceeds the size limit")

    def _consume_event(self, journal: _Journal, line: bytes):
        if not line:
            raise WorkflowError("TELEMETRY_CORRUPT", "Solver event journal contains an empty line")
        try:
            event = json.loads(line.decode("utf-8", "strict"),
                               parse_constant=lambda _value: (_ for _ in ()).throw(ValueError("non-finite number")),
                               object_pairs_hook=_unique_object)
            _finite_json(event)
        except (UnicodeError, ValueError, TypeError, RecursionError, OverflowError):
            raise WorkflowError("TELEMETRY_CORRUPT", "Solver event journal contains invalid JSON") from None
        if not isinstance(event, dict):
            raise WorkflowError("TELEMETRY_CORRUPT", "Solver event must be an object")
        sequence = event.get("sequence")
        try:
            valid_sequence = (type(sequence) in (int, float) and math.isfinite(sequence) and
                              sequence == int(sequence) and 1 <= sequence <= MAX_EXACT_INTEGER)
        except (OverflowError, ValueError):
            valid_sequence = False
        if not valid_sequence:
            raise WorkflowError("TELEMETRY_CORRUPT", "Solver event sequence is invalid")
        sequence = int(sequence)
        run_id = _uuid(event.get("runId"))
        event_id = _uuid(event.get("eventId"))
        utc = event.get("utc")
        if (event.get("schemaVersion") != 1 or type(event.get("schemaVersion")) is not int or
                run_id != journal.run_id or sequence != journal.next_event_sequence or
                event_id in journal.event_ids or not isinstance(utc, str) or len(utc) > 64 or
                not utc.endswith("Z") or not _valid_utc(utc) or
                not isinstance(event.get("type"), str) or not event["type"] or len(event["type"]) > 128 or
                not isinstance(event.get("status"), str) or not event["status"] or len(event["status"]) > 128 or
                not isinstance(event.get("payload"), dict)):
            raise WorkflowError("TELEMETRY_CORRUPT", "Solver event schema, run ID, or sequence is invalid")
        journal.event_ids.add(event_id)
        journal.event_count += 1
        if journal.event_count > MAX_EVENTS:
            raise WorkflowError("TELEMETRY_LIMIT", "Solver event journal contains too many records")
        journal.next_event_sequence += 1
        if event["type"] in ("RunFinished", "RunFailed"):
            journal.finished = True
        if event["type"] == "ControlStepAccepted":
            if event["status"] != "accepted":
                raise WorkflowError("TELEMETRY_CORRUPT", "Control step does not have accepted status")
            sample = _control_sample(sequence, event["payload"])
            if journal.samples and sample["timeS"] <= journal.samples[-1]["timeS"]:
                raise WorkflowError("TELEMETRY_CORRUPT", "Accepted control times must increase strictly")
            sample_bytes = len(line)
            # Switching between completed experiments must not exhaust the
            # shared cache merely because older traces remain cached.
            while self._cached_sample_bytes + sample_bytes > MAX_CACHED_SAMPLE_BYTES:
                old_key = next((key for key, cached in self._journals.items() if cached is not journal), None)
                if old_key is None:
                    break
                oldest = self._journals.pop(old_key)
                self._cached_sample_bytes -= oldest.sample_bytes
            if len(journal.samples) >= MAX_SAMPLES or self._cached_sample_bytes + sample_bytes > MAX_CACHED_SAMPLE_BYTES:
                raise WorkflowError("TELEMETRY_LIMIT", "Accepted control telemetry exceeds the cache limit")
            journal.samples.append(sample)
            journal.sample_bytes += sample_bytes
            self._cached_sample_bytes += sample_bytes


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _finite_json(value, depth: int = 0):
    if depth > 16:
        raise ValueError("JSON nesting limit exceeded")
    if value is None or isinstance(value, bool):
        return
    if type(value) in (int, float):
        try:
            if not math.isfinite(value):
                raise ValueError("non-finite number")
        except OverflowError:
            raise ValueError("non-finite number") from None
        return
    if isinstance(value, str):
        if len(value) > 65_536 or any(0xD800 <= ord(character) <= 0xDFFF for character in value):
            raise ValueError("text limit exceeded")
        return
    if isinstance(value, list):
        if len(value) > MAX_CHANNELS:
            raise ValueError("collection limit exceeded")
        for item in value:
            _finite_json(item, depth + 1)
        return
    if isinstance(value, dict):
        if len(value) > MAX_CHANNELS:
            raise ValueError("collection limit exceeded")
        for key, item in value.items():
            if not isinstance(key, str):
                raise ValueError("invalid JSON key")
            _finite_json(item, depth + 1)
        return
    raise ValueError("invalid JSON value")


def _uuid(value) -> str | None:
    if not isinstance(value, str):
        return None
    try:
        parsed = str(uuid.UUID(value))
    except (ValueError, AttributeError):
        return None
    return parsed if parsed == value else None


def _valid_utc(value: str) -> bool:
    try:
        datetime.fromisoformat(value[:-1] + "+00:00")
        return True
    except ValueError:
        return False


def _number(value, field_name: str, *, minimum: float | None = None) -> float:
    if type(value) not in (int, float):
        raise WorkflowError("TELEMETRY_CORRUPT", f"Control telemetry field {field_name} is invalid")
    try:
        number = float(value)
    except (OverflowError, ValueError):
        raise WorkflowError("TELEMETRY_CORRUPT", f"Control telemetry field {field_name} is invalid") from None
    if not math.isfinite(number) or abs(number) > 1e100 or (minimum is not None and number < minimum):
        raise WorkflowError("TELEMETRY_CORRUPT", f"Control telemetry field {field_name} is invalid")
    return number


def _channel_id(value) -> str:
    if not isinstance(value, str) or not _CHANNEL_ID.fullmatch(value) or value.strip() != value:
        raise WorkflowError("TELEMETRY_CORRUPT", "Control telemetry contains an invalid channel ID")
    return value


def _control_sample(sequence: int, payload: dict) -> dict:
    accepted_time = _number(payload.get("acceptedTimeS"), "acceptedTimeS", minimum=0.0)
    next_time = _number(payload.get("nextTimeS"), "nextTimeS", minimum=0.0)
    sensors = payload.get("sensors")
    applied = payload.get("appliedChannels")
    if (next_time <= accepted_time or not isinstance(sensors, list) or len(sensors) > MAX_CHANNELS or
            not isinstance(applied, list) or len(applied) > MAX_CHANNELS or
            type(payload.get("resultArtifactPersisted")) is not bool):
        raise WorkflowError("TELEMETRY_CORRUPT", "Accepted control event payload is invalid")

    projected_sensors = []
    seen = set()
    for sensor in sensors:
        if not isinstance(sensor, dict):
            raise WorkflowError("TELEMETRY_CORRUPT", "Accepted control event contains an invalid sensor")
        sensor_id = _channel_id(sensor.get("sensorId"))
        unit = sensor.get("unit")
        if (sensor_id in seen or not isinstance(unit, str) or not unit or len(unit) > 32 or
                any(ord(char) < 32 or ord(char) == 127 for char in unit)):
            raise WorkflowError("TELEMETRY_CORRUPT", "Accepted control event contains an invalid sensor unit")
        seen.add(sensor_id)
        projected_sensors.append({"id": sensor_id, "value": _number(sensor.get("value"), "sensor.value"),
                                  "unit": unit})

    projected_actuators = []
    seen.clear()
    for actuator in applied:
        if not isinstance(actuator, dict):
            raise WorkflowError("TELEMETRY_CORRUPT", "Accepted control event contains an invalid actuator")
        actuator_id = _channel_id(actuator.get("actuatorId"))
        if actuator_id in seen:
            raise WorkflowError("TELEMETRY_CORRUPT", "Accepted control event contains duplicate actuators")
        seen.add(actuator_id)
        projected_actuators.append({
            "id": actuator_id,
            "forceN": _number(actuator.get("appliedForceN"), "appliedForceN"),
            "powerW": _number(actuator.get("endpointPowerW"), "endpointPowerW"),
            "workJ": _number(actuator.get("cumulativeWorkJ"), "cumulativeWorkJ")})
    return {"seq": sequence, "timeS": accepted_time, "nextTimeS": next_time,
            "sensors": projected_sensors, "actuators": projected_actuators}
