#!/usr/bin/env python3
"""Create a new dedicated Cortex Remote CA and separate agent/client identities.

OpenSSL is a provisioning dependency only, not a runtime requirement. Private
keys are generated locally, never embedded in Cortex or committed to Git.
The output directory must not already exist. Keep authority/ offline; copy
only agent/ to the target and client/ to the PC. This does not deploy anything.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def create(out: Path, endpoint: str, platform: str = 'ps5', server_name: str = 'cortex-agent.local', days: int = 30) -> None:
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]{0,252}', server_name):
        raise ValueError('server_name must be a simple DNS certificate name')
    if platform not in ('ps5', 'freebsd', 'fixture') or not 1 <= days <= 365:
        raise ValueError('invalid platform or certificate lifetime')
    match = re.fullmatch(r'([0-9]+(?:\.[0-9]+){3}):([0-9]+)', endpoint)
    if not match or not 1 <= int(match[2]) <= 65535 or any(int(x) > 255 or (len(x) > 1 and x[0] == '0') for x in match[1].split('.')):
        raise ValueError('endpoint must be numeric IPv4:port')
    openssl = os.environ.get('CORTEX_OPENSSL') or shutil.which('openssl')
    if not openssl:
        raise RuntimeError('OpenSSL is required to generate certificates')
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=False, mode=0o700)
    for name in ('authority', 'agent', 'client'):
        (out / name).mkdir(mode=0o700)

    def run(*args: str) -> None:
        proc = subprocess.run([openssl, *map(str, args)], cwd=out, capture_output=True, text=True, timeout=30)
        if proc.returncode:
            raise RuntimeError('OpenSSL failed: ' + proc.stderr[-2000:])

    run('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes',
        '-keyout', 'authority/ca.key', '-out', 'authority/ca.crt', '-days', str(days), '-sha256',
        '-subj', '/CN=Cortex Remote Dedicated CA', '-addext', 'basicConstraints=critical,CA:TRUE,pathlen:0',
        '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
    for side, purpose, common in [('agent', 'serverAuth', server_name), ('client', 'clientAuth', 'cortex-client')]:
        run('req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes',
            '-keyout', f'{side}/identity.key', '-out', f'{side}/identity.csr', '-subj', '/CN=' + common)
        ext = 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=' + purpose + '\n'
        if side == 'agent':
            ext += 'subjectAltName=DNS:' + server_name + '\n'
        (out / side / 'extensions.txt').write_text(ext, encoding='ascii')
        run('x509', '-req', '-in', f'{side}/identity.csr', '-CA', 'authority/ca.crt', '-CAkey', 'authority/ca.key',
            '-CAcreateserial', '-out', f'{side}/identity.crt', '-days', str(days), '-sha256', '-extfile', f'{side}/extensions.txt')
        shutil.copyfile(out / 'authority/ca.crt', out / side / 'ca.crt')
        (out / side / 'identity.csr').unlink()
        (out / side / 'extensions.txt').unlink()
    common = {'ca_certificate': 'ca.crt', 'certificate': 'identity.crt', 'private_key': 'identity.key', 'allow_writes': False, 'timeout_ms': 5000}
    client = dict(common, schema='cortex.remote.client.v1', endpoint=endpoint, server_name=server_name, expected_platform=platform)
    agent = dict(common, schema='cortex.remote.agent.v1', listen=endpoint, allow_ptrace_stop=False, allowed_pids=[])
    for side, config in [('agent', agent), ('client', client)]:
        (out / side / (side + '.json')).write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')
    if os.name != 'nt':
        for path in out.rglob('*'):
            if path.is_file():
                path.chmod(0o600)
    # Prevent accidental git add from inside a generated credentials directory.
    (out / '.gitignore').write_text('*\n', encoding='ascii')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--platform', choices=('ps5', 'freebsd', 'fixture'), default='ps5')
    parser.add_argument('--server-name', default='cortex-agent.local')
    parser.add_argument('--days', type=int, default=30)
    args = parser.parse_args()
    create(args.out, args.endpoint, args.platform, args.server_name, args.days)
    print('Created separate agent/client identities in', args.out.resolve())
    print('Keep authority/ offline. Restrict directory ACLs to the operator on Windows.')
    print('Select allowed_pids in agent.json and deploy agent/ yourself; writes are disabled.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
