# WF12 native LPAC launcher checkpoint

This checkpoint describes independently authored source, not an accepted isolation backend. Ordinary generated-code execution remains OFF until the root integrates the adapter, compiles the installed binary, runs the dedicated counterexamples, and completes independent review. No build, probe, AppContainer launch, or acceptance test was run by the implementation agent while the root's benchmark was active.

## Ownership and entry points

The implementation owns only `src/range-workflow/native/isolation_launcher.cpp`, its header, and this checkpoint. The root owns CMake, installation, Python staging/lifecycle/recovery, cancellation integration, tests, and acceptance records. Link Qt6 Core, Kernel32, Advapi32, Userenv, FirewallAPI, Shell32, and Ole32. SDK `INITKNOWNFOLDERS` instantiates the known-folder identifier definitions locally. The source targets Windows SDK 10.0.26100.0 and MSVC; it records `_MSC_FULL_VER` and an SDK contract string. The latter is a build declaration, not a runtime measurement. No supported product OS matrix is inferred from an SDK enum or a successful probe.

The standalone executable has two fixed entry points:

```text
range-workflow-isolation-launcher --probe
range-workflow-isolation-launcher run --request D:\host-managed\task-id\job\request.json
```

Probe reports schema `range.workflow.lpac.probe/v1`, backend `windows-lpac/v1`, the observed OS build, limits, `available=false`, and `requiresLaunchVerification=true`. It never proves a token, startup attribute, DLL import, denial predicate, or installed-runtime compatibility. A real run must verify those separately.

## Request contract

The request is a host-owned local regular file, limited to 64 KiB, held open without write/delete sharing. Its flat JSON object accepts exactly these keys; duplicate, unknown, missing, trailing, and wrong-type fields fail closed:

| Key | Contract |
| --- | --- |
| `schema` | `range.workflow.lpac.request/v1` |
| `managedRoot` | Existing host-managed staging ancestor; never a user-supplied ACL target |
| `jobRoot` | Existing descendant of `managedRoot`, outside the staged runtime |
| `runtimeRoot` | New disposable staged copy below `managedRoot`, outside `jobRoot` |
| `harnessPath` | Regular file below `jobRoot`, basename exactly `extension_child_v2.py` |
| `sourcePath`, `inputPath` | Separate regular files below `jobRoot` |
| `outputRoot` | Nonexistent direct child of `jobRoot`; launcher creates it exclusively |
| `mode` | Exactly `run` or `test` |
| `timeoutMs` | Integer 1 through 30000 |
| `pythonSha256`, `runtimeSha256`, `harnessSha256`, `contractSha256`, `sourceSha256`, `inputSha256` | Lowercase SHA256 strings, computed by the host after approval binding |

The request itself must be below `jobRoot`. `scientific_contract.py` is the fixed adjacent harness dependency. Python is always `runtimeRoot\python.exe`; output is always `outputRoot\result.json`. The launcher does not accept an executable, arbitrary argv, `-c`, capabilities, inherited environment, or configurable resource ceilings. The argv is exactly:

```text
python.exe -I extension_child_v2.py --source sourcePath --input inputPath --output outputRoot\result.json --run|--test
```

The root must independently verify the fixed harness/contract hashes against the trusted installation before building this request. Comparing caller-provided hashes proves content binding, not provenance or approval. The launcher owner check does not establish that a runtime copy is new; the root must create task-unique staging from a verified frozen private runtime and never direct ACL changes at an installed/shared runtime.

`runtimeSha256` covers every regular file, including runtime locks and `.pth` files. Normalize relative paths to forward slashes; sort their UTF-8 bytes; SHA256 the concatenation, for each file, of relative-path UTF-8, NUL, its lowercase file SHA256 ASCII, LF. Empty directories are not content records. No exclusions. Limit 8192 files, 16384 total entries, depth 32, and total file bytes 2 GiB. The launcher pins and hashes the files itself. Individual source/harness files are at most 64 KiB, contract at most 1 MiB, and input at most 32 MiB.

## Security and startup sequence

Local absolute DOS paths only; alternate streams, device/UNC paths, reserved DOS names, ambiguous components, and every encountered junction/symlink/reparse point are rejected. Ancestor directory handles and final file/directory handles remain open without delete sharing. Final names from `GetFinalPathNameByHandleW` must match the requested local path; containment checks occur only after that verification. Regular files must have one hard link. Runtime files are pinned without write sharing. Output monitoring pins each enumerated directory before recursion.

Before ACL changes, save `jobRoot\wf12-cleanup.json`, schema `range.workflow.lpac.cleanup/v1`, with the unique profile name and all original DACL SDDL strings (plus output label). Then create a new `RangeWF12.<UUID>` profile, never reuse an existing profile, and apply protected ACLs only to disposable owner-verified runtime paths, the four readonly job files, and fresh output. Runtime/input SID permissions are read/execute. Output SID permissions are read/write/execute/delete, excluding WRITE_DAC and WRITE_OWNER; output ACEs inherit to children, including explicit host/system/admin access. An OWNER RIGHTS read-control ACE suppresses implicit owner WRITE_DAC. Output receives an inheritable low-integrity no-write-up label. Ancestor user directories, source private runtime, `.agents`, provider settings, and actual credentials are not ACL targets.

Profile creation has zero capabilities. Before creation of the process, configure:

1. `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` with the new SID and zero capabilities.
2. `PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY` with OPT_OUT, requiring LPAC.
3. `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY` with CHILD_PROCESS_RESTRICTED.
4. `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` with exactly null stdin, stdout pipe, and stderr pipe.
5. `PROC_THREAD_ATTRIBUTE_JOB_LIST` with the configured private job, assigning the worker during creation rather than leaving a creation-to-assignment crash window.

The Job Object sets kill-on-close, one active process, 512 MiB per-process/job commit, and 15 seconds of per-process/job **user CPU time**. No breakaway flag. Create the worker suspended with Unicode allowlisted environment and no window, already assigned to the configured job. Query actual job membership/limits and actual token AppContainer status, LPAC status, exact package SID, zero capabilities, low integrity RID, and restricted child-process flags. Resume only after every value matches. Failure at any step terminates the suspended worker and fails; there is no ordinary AppContainer or trusted-process fallback. Loopback exemptions are read with `NetworkIsolationGetAppContainerConfig`, and the unique SID must be absent.

The environment explicitly contains only SystemRoot, WINDIR, OMP_NUM_THREADS=1, OPENBLAS_NUM_THREADS=1, and TEMP/TMP pointing to output. No PATH, session/provider variables, PYTHONPATH, or credential variables are copied. Windows profile defaults may add/redirect worker environment values; the receipt deliberately reports `environmentObserved=null`, rather than pretending the declared block proves remote environment contents. A dedicated fixture must inspect the actual worker environment.

## Limits, output, and receipts

The worker wall-clock deadline is at most 30 seconds. Combined stdout/stderr is at most 4 MiB, so its base64 receipt fits the existing host 8 MiB subprocess-output bound. Output is polled every 10 ms for aggregate size at most 32 MiB, at most 128 entries and depth 8, rejecting reparses. This is **not a hard disk quota**: a worker can transiently write more between polls, and profile storage has no disk quota. The profile has its own writable storage in addition to allowed output. Some common system resources remain accessible according to Windows policy. No capability or counterexample claim means that all system paths/registry objects are blocked.

After the worker exits, terminate the job before final pipe/file validation. Validate the result is a nonempty regular single-link file below output, at most 32 MiB, parses as a JSON object, and calculate its byte count/hash. The root must reopen/recheck final path, size, strict JSON, schema, units/types, and content hash before using it. No worker result JSON is embedded in the launcher receipt.

Stdout is one compact JSON receipt, schema `range.workflow.lpac.receipt/v1`; nonzero exit means failure. Success includes status/code, frozen declared and observed hashes, observed OS/compiler declaration, actualToken, actual job readback, output path/bytes/hash, bounded stdout/stderr base64, profile, cleanupManifest, and cleanupComplete. Dependency versions 3.13.5/2.2.6/1.15.3 are explicitly declared; observed versions are null. The fixed v2 harness checks NumPy/SciPy versions, but an acceptance fixture must separately bind runtime provenance and measured versions. A profile/ACL cleanup failure changes status to failed/CLEANUP_REQUIRED and prevents activation.

Cancellation means the host terminates the launcher process under its existing ProcessRunner kill-on-close job. The launcher's noninherited job handle closes on exit/crash, terminating the worker. This is a lifecycle contract requiring an actual cancellation/process-survival test, not evidence produced by source inspection. There is no worker-accessible cancellation event or extra IPC handle.

## Recovery and pending acceptance

Normal cleanup restores original ACLs through retained handles and deletes the unique AppContainer profile without deleting scientific output. The manifest state becomes complete or recovery_required. Cleanup during exception unwinding also updates the receipt with actual ACL restoration attempts/failures, profile deletion HRESULT, and manifest-save status. `phase`/`failurePhase` preserve the startup/worker failure location; `processCreation` records the immediate BOOL/error result. Before any ACL/profile mutation, `cleanupComplete=true` and cleanup state not_required mean no such cleanup is needed, not a successful worker run. If the launcher dies, its job handle kills the worker, but profile/ACL restoration requires root recovery. Before deleting/resetting disposable staging, the root must validate the recovery file and all targets against the task-owned managed roots and reject reparses. Restore the recorded DACL/label with host privileges or delete the disposable staging after workers are dead; call DeleteAppContainerProfile for the exact recorded unique profile. Never execute a manifest-supplied arbitrary path or treat profile cleanup failure as success. A prepared manifest may name a profile whose creation never completed; absence is a valid recovery condition. Existing cleanup manifests block a new launch in the same job directory.

## First smoke failure diagnosis (2026-10-09, not acceptance)

The root's retained `build/audit-20261009/WF-10/lpac-path-normalized-run.log` reports WIN32_203 at CreateProcessW after matching all six hashes and applying the profile/ACL/startup configuration. [Microsoft's error-code contract](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-) identifies 203 as ERROR_ENVVAR_NOT_FOUND. The old receipt says cleanupComplete=false, while the retained task `a50239c1-df51-4491-926d-340d06b787b2/job/wf12-cleanup.json` reports state complete for 2682 paths. This establishes an exception-cleanup reporting defect, not a worker sandbox pass.

Inspection of the current `wide()` helper shows a direct QString UTF-16 pointer, no reused thread_local buffer. Python and output-root QString owners stay alive throughout the call; shared-buffer overwriting is not established as the cause of 203. The source now uses distinct named std::wstring owners for application and current-directory arguments, captures CreateProcessW's BOOL and immediate GetLastError after clearing the previous error, and updates exception cleanup evidence in the receipt. No environment entry, capability, handle, Job limit, or LPAC gate was relaxed. The original cause of 203 remains unverified; a fresh staged fixture under the rebuilt launcher is required. Microsoft documents [AppContainer profile environment redirection](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer), but does not identify a required missing environment key for this failure; that remains a hypothesis until separately observed. Do not reuse the old job, edit its recovery record, or treat its failure as acceptance evidence. The diagnosing implementation agent ran no build, native process, or AppContainer test during the root's subsequent scientific story.

## Controlled caller LOCALAPPDATA diagnostic

The root's second fresh fixture, `WF-12/lpac-create-diagnostic/isolated-workers/33f5f3bf-816e-4074-8bc0-e91632b00fce/job/lpac-launcher.log`, confirms immediate CreateProcessW false/error 203 at create_suspended_worker. The updated receipt reports complete cleanup: 2682 ACL restoration attempts, zero failures, profile deletion HRESULT 0, and manifest saved. Thus stale LastError and the previous cleanup-report defect do not explain the remaining startup failure.

A [first-party launcher investigation](https://github.com/Convira/convira-sandbox/issues/1#issuecomment-5225682721) reports a controlled Windows experiment in which adding LOCALAPPDATA alone to a stripped environment restored AppContainer startup, while APPDATA, USERPROFILE, SystemRoot/SystemDrive, and TEMP/TMP did not. Its launcher inherited the caller environment, so that experiment does not distinguish caller environment from the separately supplied worker environment used here. It is a diagnostic hypothesis, not a Microsoft compatibility guarantee or evidence that this launcher works.

With explicit root authorization, the next source revision makes exactly one intervention: immediately before CreateProcessW, use `SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, ...)` to derive the current user's registered LocalAppData path, temporarily set **only the launcher's own** LOCALAPPDATA variable, and restore its prior presence/value immediately after the API result/error is captured. RAII retries restoration on exceptions; exact readback checks restoration. Known-folder memory is freed with CoTaskMemFree. The worker continues receiving the unchanged six-variable explicit environment; no full environment or credential/session variable is inherited. No SID ACL is applied to the current-user known folder or any other real user path. The only writable profile storage remains the new AppContainer's Windows-created storage already disclosed above.

`callerEnvironment` records source API and presence before/during/after, restore attempt/error, and exact-restoration success; it omits all path values. This source-only change has not been built or run by the implementation agent. The root must use one fresh task-owned fixture, keep all LPAC/Job/ACL/handle gates fixed, and preserve the outcome. If the caller variable was already present and the same 203 remains, this intervention has not solved startup; do not generalize the unrelated project's experiment into acceptance. Any subsequent explicit-worker LOCALAPPDATA initialization requires a separate bounded decision and observation. Ordinary mode remains OFF.

Pending: compile under SDK 10.0.26100.0/v145; check token result formats on the actual OS; test native Python/NumPy/SciPy loading with zero LPAC capabilities; fixture-only private-file/credential, network, registry, child-process, ACL-owner escalation, input write, output reparse/hard-link, memory/CPU/output/time/cancellation denial and positive output cases; crash/profile recovery; clean Windows installation; generated-function isolated story; independent review. Do not loosen LPAC/token/resource checks to make a fixture pass. Save exact failure evidence and keep ordinary mode unavailable if the runtime requires unavailable rights.

## Primary API references consulted during implementation

Independent implementation based on API contracts; no external example code was copied.

- [Launch an AppContainer / LPAC](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)
- [UpdateProcThreadAttribute](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)
- [CreateAppContainerProfile](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-createappcontainerprofile)
- [TOKEN_INFORMATION_CLASS](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-token_information_class)
- [GetTokenInformation](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation)
- [JOBOBJECT_EXTENDED_LIMIT_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information)
- [GetFinalPathNameByHandleW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)
- [SetSecurityInfo](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-setsecurityinfo)
- [NetworkIsolationGetAppContainerConfig](https://learn.microsoft.com/en-us/windows/win32/api/netfw/nf-netfw-networkisolationgetappcontainerconfig)
- [Owner Rights SID](https://learn.microsoft.com/en-us/windows-server/identity/ad-ds/manage/understand-security-identifiers)
- [Object owner implicit WRITE_DAC](https://learn.microsoft.com/en-us/windows/win32/secauthz/owner-of-a-new-object)
- [CreateProcessW argument/environment/error contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
- [Win32 error codes 0–499](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-)
- [SHGetKnownFolderPath current-user path and allocation contract](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath)
- [SetEnvironmentVariableW process-local scope](https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-setenvironmentvariablew)
- [GetEnvironmentVariableW result and absence contract](https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentvariablew)
- [GetAppContainerFolderPath](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-getappcontainerfolderpath)
