"""Safe subprocess client for the native document command service."""

from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from typing import Any, Mapping


class NativeDocumentError(RuntimeError):
    """A structured rejection or protocol failure from range-model-command."""

    def __init__(self, response: Mapping[str, Any] | None = None, *,
                 stderr: str = "", returncode: int | None = None, message: str = ""):
        self.response = dict(response or {})
        self.stderr = stderr
        self.returncode = returncode
        self.code = self.response.get("code", "native_cli_error")
        detail = message or self.response.get("message") or stderr.strip() or "Native document command failed"
        super().__init__(f"{self.code}: {detail}")


def _canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
                      allow_nan=False)


@dataclass(frozen=True)
class NativeCommandPreview:
    """Immutable preview binding for one explicit caller-directed execute call.

    The client creates this value after the native service returns a valid preview.
    It is not an approval token and has no method that accepts or publishes itself.
    """

    source_sha256: str
    command_id: str
    fingerprint: str
    digest: str
    _arguments_json: str = field(repr=False)
    _owner_token: object = field(repr=False, compare=False)

    @property
    def normalized_arguments(self) -> dict[str, Any]:
        return json.loads(self._arguments_json)


class NativeDocumentClient:
    """Call the native CLI with JSON files and argument arrays, never a shell."""

    def __init__(self, cli_path: str | Path, source_path: str | Path, *, timeout: float = 30):
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        executable = str(cli_path)
        self.cli_path = (str(Path(executable).resolve())
                         if Path(executable).is_absolute() or "/" in executable or "\\" in executable
                         else executable)
        self.source_path = str(Path(source_path).resolve())
        self.timeout = timeout
        self._preview_token = object()

    def _run(self, *options: str) -> dict[str, Any]:
        try:
            completed = subprocess.run([self.cli_path, "--input", self.source_path, *options],
                                       capture_output=True, text=True, timeout=self.timeout,
                                       check=False, shell=False, encoding="utf-8")
        except (OSError, subprocess.TimeoutExpired) as error:
            raise NativeDocumentError(message=f"Could not run the native CLI: {error}") from error
        try:
            response = json.loads(completed.stdout)
        except (json.JSONDecodeError, TypeError) as error:
            raise NativeDocumentError(stderr=completed.stderr, returncode=completed.returncode,
                                      message="Native CLI stdout was not one JSON document") from error
        if not isinstance(response, dict):
            raise NativeDocumentError(stderr=completed.stderr, returncode=completed.returncode,
                                      message="Native CLI JSON result must be an object")
        if completed.returncode != 0 or response.get("status") == "rejected":
            raise NativeDocumentError(response, stderr=completed.stderr,
                                      returncode=completed.returncode)
        return response

    def _with_json_file(self, option: str, value: Mapping[str, Any], *extra: str) -> dict[str, Any]:
        try:
            encoded = _canonical_json(dict(value))
        except (TypeError, ValueError) as error:
            raise ValueError(f"Request must be finite JSON data: {error}") from error
        with tempfile.TemporaryDirectory(prefix="range-native-document-") as directory:
            request_path = Path(directory) / "request.json"
            request_path.write_text(encoded, encoding="utf-8")
            return self._run(*extra, option, str(request_path))

    @staticmethod
    def _source_sha(response: Mapping[str, Any]) -> str:
        value = response.get("sourceSha256")
        if (not isinstance(value, str) or len(value) != 64
                or any(character not in "0123456789abcdef" for character in value)):
            raise NativeDocumentError(response, message="Native CLI did not return a source SHA256")
        return value

    def query(self) -> dict[str, Any]:
        return self._run("--query")

    def capabilities(self) -> dict[str, Any]:
        current = self.query()
        response = self._run("--capabilities")
        if self._source_sha(response) != self._source_sha(current):
            raise NativeDocumentError(response, message="Native source changed while reading capabilities")
        return response

    def context(self, page: Mapping[str, Any] | None = None) -> dict[str, Any]:
        return self._read_page("--context", page)

    def targets(self, page: Mapping[str, Any] | None = None) -> dict[str, Any]:
        return self._read_page("--targets", page)

    def _read_page(self, action: str, page: Mapping[str, Any] | None) -> dict[str, Any]:
        current = self.query()
        request = dict(page or {})
        if not request:
            response = self._run(action)
        else:
            response = self._with_json_file("--page", request, action)
        if self._source_sha(response) != self._source_sha(current):
            raise NativeDocumentError(response, message="Native source changed while reading its target page")
        return response

    def preview(self, command_id: str, arguments: Mapping[str, Any]) -> NativeCommandPreview:
        return self._preview_for_source(command_id, arguments, None)

    def _preview_for_source(self, command_id: str, arguments: Mapping[str, Any],
                            expected_source_sha: str | None) -> NativeCommandPreview:
        if not isinstance(command_id, str) or not command_id:
            raise ValueError("command_id must be a nonempty string")
        current = self.query()
        source_sha = self._source_sha(current)
        if expected_source_sha is not None and source_sha != expected_source_sha:
            raise NativeDocumentError(current, message="Native source changed while preparing the command")
        response = self._command_request(command_id, arguments, source_sha, preview=True)
        if response.get("status") != "preview":
            raise NativeDocumentError(response, message="Native service did not return a command preview")
        normalized = response.get("normalizedParameters", dict(arguments))
        if not isinstance(normalized, dict):
            raise NativeDocumentError(response, message="Native preview returned invalid normalized parameters")
        fingerprint = response.get("fingerprint", "")
        if not isinstance(fingerprint, str):
            raise NativeDocumentError(response, message="Native preview returned an invalid fingerprint")
        binding = {"sourceSha256": source_sha, "commandId": command_id,
                   "normalizedArguments": normalized, "fingerprint": fingerprint}
        digest = hashlib.sha256(_canonical_json(binding).encode("utf-8")).hexdigest()
        return NativeCommandPreview(source_sha, command_id, fingerprint, digest,
                                    _canonical_json(normalized), self._preview_token)

    def execute(self, preview: NativeCommandPreview, output_path: str | Path, *,
                journal_path: str | Path | None = None) -> dict[str, Any]:
        if not isinstance(preview, NativeCommandPreview) or preview._owner_token is not self._preview_token:
            raise ValueError("execute requires a preview produced by this NativeDocumentClient")
        arguments = preview.normalized_arguments
        binding = {"sourceSha256": preview.source_sha256, "commandId": preview.command_id,
                   "normalizedArguments": arguments, "fingerprint": preview.fingerprint}
        expected_digest = hashlib.sha256(_canonical_json(binding).encode("utf-8")).hexdigest()
        if expected_digest != preview.digest:
            raise ValueError("Native command preview binding is invalid")
        current = self.query()
        if self._source_sha(current) != preview.source_sha256:
            raise NativeDocumentError(current, message="Native source changed after preview")
        options: list[str] = ["--output", str(Path(output_path).resolve())]
        if journal_path is not None:
            options.extend(("--journal", str(Path(journal_path).resolve())))
        response = self._command_request(preview.command_id, arguments, preview.source_sha256,
                                         preview=False, extra_options=options)
        if response.get("status") != "published":
            raise NativeDocumentError(response, message="Native command did not publish a verified model")
        if self._source_sha(response) != preview.source_sha256:
            raise NativeDocumentError(response, message="Published command reports a different source SHA256")
        normalized = response.get("normalizedParameters")
        if isinstance(normalized, dict) and normalized != arguments:
            raise NativeDocumentError(response, message="Published command differs from its frozen preview")
        return response

    def _command_request(self, command_id: str, arguments: Mapping[str, Any], source_sha: str,
                         *, preview: bool, extra_options: list[str] | None = None) -> dict[str, Any]:
        try:
            copied = json.loads(_canonical_json(dict(arguments)))
        except (TypeError, ValueError) as error:
            raise ValueError(f"Command arguments must be finite JSON data: {error}") from error
        if not isinstance(copied, dict):
            raise ValueError("Command arguments must be an object")
        envelope = {"sourceSha256": source_sha, "parameters": copied}
        options = ["--command", command_id, "--dry-run"] if preview else ["--command", command_id]
        if extra_options:
            options.extend(extra_options)
        return self._with_json_file("--parameters", envelope, *options)

    def _command_spec(self, command_id: str, expected_source_sha: str | None = None) -> dict[str, Any]:
        response = self.capabilities()
        if expected_source_sha is not None and self._source_sha(response) != expected_source_sha:
            raise NativeDocumentError(response, message="Native source changed while reading command capabilities")
        commands = response.get("commands")
        if not isinstance(commands, list):
            raise NativeDocumentError(response, message="Capabilities did not include command descriptions")
        for command in commands:
            if isinstance(command, dict) and command.get("id") == command_id:
                if command.get("supported") is not True:
                    raise ValueError(f"Native command is not supported for this source: {command_id}")
                return command
        raise ValueError(f"Native command is not advertised: {command_id}")

    @staticmethod
    def _check_fields(spec: Mapping[str, Any], arguments: Mapping[str, Any]) -> None:
        required = spec.get("required")
        defaults = spec.get("optionalDefaults", {})
        if not isinstance(required, list) or not isinstance(defaults, dict):
            raise NativeDocumentError(spec, message="Command capability has an invalid argument description")
        expected = set(required) | set(defaults)
        if set(arguments) != expected:
            missing = sorted(expected - set(arguments))
            extra = sorted(set(arguments) - expected)
            raise ValueError(f"Command fields do not match capabilities; missing={missing}, unexpected={extra}")

    def configure_modal(self, *, method: str, iterations: int, modes: int,
                        tolerance: float) -> NativeCommandPreview:
        command = "analysis.modal.configure"
        source_sha = self._source_sha(self.query())
        spec = self._command_spec(command, source_sha)
        arguments = {"method": method, "iterations": iterations, "modes": modes,
                     "tolerance": tolerance, "unit": spec.get("units", {}).get("tolerance")}
        self._check_fields(spec, arguments)
        if method not in spec.get("methods", []):
            raise ValueError("Modal method is not advertised by the native service")
        return self._preview_for_source(command, arguments, source_sha)

    def configure_structural_dynamics(self, *, enabled: bool, method: str,
                                      time_step: float, steps: int, output_frequency: int,
                                      rayleigh_alpha: float, rayleigh_beta: float,
                                      rho_infinity: float, newmark_gamma: float,
                                      newmark_beta: float,
                                      modal_damping_ratios: list[float] | tuple[float, ...]) -> NativeCommandPreview:
        command = "analysis.structural_dynamics.configure"
        source_sha = self._source_sha(self.query())
        spec = self._command_spec(command, source_sha)
        arguments = {"enabled": enabled, "method": method, "timeStep": time_step,
                     "unitSI": True, "steps": steps, "outputFrequency": output_frequency,
                     "rayleighAlpha": rayleigh_alpha, "rayleighBeta": rayleigh_beta,
                     "rhoInfinity": rho_infinity, "newmarkGamma": newmark_gamma,
                     "newmarkBeta": newmark_beta,
                     "modalDampingRatios": list(modal_damping_ratios)}
        self._check_fields(spec, arguments)
        if method not in spec.get("methods", []):
            raise ValueError("Structural dynamics method is not advertised by the native service")
        return self._preview_for_source(command, arguments, source_sha)

    def set_material(self, *, target_id: str, name: str, young_modulus_pa: float,
                     density_kg_m3: float, poisson_ratio: float,
                     thermal_expansion_per_k: float,
                     reference_temperature_k: float | None = None) -> NativeCommandPreview:
        command = "entity.material.set"
        source_sha = self._source_sha(self.query())
        spec = self._command_spec(command, source_sha)
        self._require_target(target_id, expected_source_sha=source_sha)
        defaults = spec.get("optionalDefaults", {})
        if not isinstance(defaults, dict):
            raise NativeDocumentError(spec, message="Material capability has invalid defaults")
        if reference_temperature_k is None:
            reference_temperature_k = defaults.get("referenceTemperatureK")
        arguments = {"targetId": target_id, "name": name, "youngModulusPa": young_modulus_pa,
                     "densityKgM3": density_kg_m3, "poissonRatio": poisson_ratio,
                     "thermalExpansionPerK": thermal_expansion_per_k}
        if "referenceTemperatureK" in defaults:
            arguments["referenceTemperatureK"] = reference_temperature_k
        self._check_fields(spec, arguments)
        return self._preview_for_source(command, arguments, source_sha)

    def set_boundary(self, *, target_id: str, kind: str,
                     values: list[float | None] | tuple[float | None, ...],
                     time_s: float | None = None) -> NativeCommandPreview:
        command = "entity.boundary.set"
        source_sha = self._source_sha(self.query())
        spec = self._command_spec(command, source_sha)
        self._require_target(target_id, kind=kind, expected_source_sha=source_sha)
        if kind not in spec.get("kinds", []):
            raise ValueError("Boundary kind is not advertised by the native service")
        units = spec.get("units", {})
        unit = units.get(kind) if isinstance(units, dict) else None
        arguments: dict[str, Any] = {"targetId": target_id, "kind": kind,
                                     "values": list(values), "unit": unit}
        defaults = spec.get("optionalDefaults", {})
        if "timeS" in defaults:
            arguments["timeS"] = defaults["timeS"] if time_s is None else time_s
        self._check_fields(spec, arguments)
        return self._preview_for_source(command, arguments, source_sha)

    def find_targets(self, label_query: str) -> dict[str, Any]:
        """Return all label matches and candidate IDs; never select a match."""
        return self.targets({"labelQuery": label_query, "limit": 200})

    def _require_target(self, target_id: str, *, kind: str | None = None,
                        expected_source_sha: str | None = None) -> dict[str, Any]:
        if not isinstance(target_id, str) or not target_id.startswith("g:"):
            raise ValueError("target_id must be a physical group ID returned by targets(), such as g:12")
        capabilities = self.capabilities()
        if expected_source_sha is not None and self._source_sha(capabilities) != expected_source_sha:
            raise NativeDocumentError(capabilities, message="Native source changed while reading target capabilities")
        pagination = capabilities.get("targetPagination", {})
        limit = pagination.get("maximumLimit", 200) if isinstance(pagination, dict) else 200
        if type(limit) is not int or not 1 <= limit <= 200:
            raise NativeDocumentError(capabilities, message="Capabilities returned an invalid target page limit")
        offset = 0
        while True:
            page = self.targets({"offset": offset, "limit": limit})
            if expected_source_sha is not None and self._source_sha(page) != expected_source_sha:
                raise NativeDocumentError(page, message="Native source changed while resolving a target")
            targets = page.get("targets")
            if not isinstance(targets, list):
                raise NativeDocumentError(page, message="Target page has no target list")
            for target in targets:
                if isinstance(target, dict) and target.get("id") == target_id:
                    if kind == "force" and target.get("type") != "point":
                        raise ValueError("Force boundaries require a Point target")
                    return target
            if page.get("hasMore") is not True:
                break
            offset += len(targets)
            if not targets:
                raise NativeDocumentError(page, message="Target pagination made no progress")
        raise ValueError(f"Target does not exist in the current native source: {target_id}")
