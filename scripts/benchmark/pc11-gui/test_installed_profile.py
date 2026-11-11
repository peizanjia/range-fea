"""Exercise installed startup rejection using isolated, retained audit fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest_tree(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    output = args.output.absolute()
    output.mkdir(parents=True, exist_ok=False)
    outside = output / 'outside'
    outside.mkdir()
    (outside / 'sentinel.ini').write_text('[guard]\nvalue=unchanged\n', encoding='utf-8')
    before = digest_tree(outside)
    results = []
    cases = [('settings', 'junction'), ('system-settings', 'junction'),
             ('application-data', 'junction'), ('application-data/session', 'junction'),
             ('settings/Range Software/Range FEA.ini', 'symlink'),
             ('application-data', 'file')]
    for index, (relative, kind) in enumerate(cases):
        profile = output / ('case-' + str(index))
        profile.mkdir()
        alias = profile / relative
        alias.parent.mkdir(parents=True, exist_ok=True)
        record = {'path': relative, 'kind': kind}
        if kind == 'junction':
            # Fixed PowerShell source, path arguments passed as environment values.
            env = dict(os.environ, RANGE_PROFILE_TEST_ALIAS=str(alias),
                       RANGE_PROFILE_TEST_TARGET=str(outside))
            created = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive',
                                      '-Command', '$ErrorActionPreference="Stop"; '
                                      'New-Item -ItemType Junction -Path $env:RANGE_PROFILE_TEST_ALIAS '
                                      '-Target $env:RANGE_PROFILE_TEST_TARGET | Out-Null'],
                                     env=env, capture_output=True, text=True)
            if created.returncode:
                record.update(status='FAIL', error=created.stderr)
                results.append(record)
                continue
        elif kind == 'symlink':
            try:
                os.symlink(outside / 'sentinel.ini', alias)
            except OSError as exc:
                record.update(status='SKIP', reason=str(exc))
                results.append(record)
                continue
        else:
            alias.write_text('ordinary file blocks managed directory', encoding='utf-8')
        env = dict(os.environ, RANGE_APPLICATION_PROFILE_DIR=str(profile))
        try:
            run = subprocess.run([str(executable), '--help'], env=env,
                                 capture_output=True, text=True, timeout=15,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
            unchanged = digest_tree(outside) == before
            record.update(exitCode=run.returncode, stderr=run.stderr,
                          outsideUnchanged=unchanged,
                          status='PASS' if run.returncode == 2 and unchanged else 'FAIL')
        except subprocess.TimeoutExpired:
            record.update(status='FAIL', error='Invalid profile was not rejected within 15 seconds')
        results.append(record)
    report = {'executable': str(executable),
              'executableSha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
              'cases': results}
    (output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(results, indent=2))
    return int(any(row['status'] == 'FAIL' for row in results))


if __name__ == '__main__':
    raise SystemExit(main())
