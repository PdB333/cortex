#!/usr/bin/env python3
"""Read-only integration of the next artifact formats through real Cortex.

Only Cortex and this test client are executed. Analyzed archive members,
extension scripts, installers, type libraries and certificates are never
executed, installed, activated or imported into trust stores. No write flag.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from formats_wave1_e2e import Mcp


CASES = [
    ('archive-next/ustar.tar', 'tar'), ('archive-next/gnu.tar', 'tar'),
    ('archive-next/plain.gz', 'gzip'), ('archive-next/package.tgz', 'gzip'),
    ('archive-next/package.tar', 'npm-tar'), ('archive-next/gnu.a', 'ar'),
    ('archive-next/sample.deb', 'deb'),
    ('certificates/certificate.der', 'x509'), ('certificates/bundle.pem', 'pem-certificates'),
    ('certificates/signed.p7m', 'pkcs7'), ('certificates/certificates.p7b', 'pkcs7'),
    ('certificates/catalog.cat', 'pkcs7-catalog'),
    ('typelib/microsoft-win32.tlb', 'tlb-msft'), ('typelib/microsoft-win64.tlb', 'tlb-msft'),
    ('package-next/sample.xpi', 'xpi'), ('package-next/sample.vsix', 'vsix'),
    ('package-next/sample2.crx', 'crx2'), ('package-next/sample3.crx', 'crx3'),
    ('package-next/jdk.jmod', 'jmod'), ('package-next/sample.war', 'war'),
    ('package-next/sample.ear', 'ear'), ('package-next/sample.pyz', 'python-zipapp'),
    ('package-next/sample.whl', 'wheel'), ('package-next/RELEASES', 'squirrel-releases'),
]


def fixture_hashes(root: Path) -> dict[str, str]:
    result = {}
    for folder in ('archive-next', 'certificates', 'typelib', 'package-next'):
        for path in sorted((root / folder).rglob('*')):
            if path.is_file():
                result[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cortex', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, default=Path(__file__).resolve().parent / 'fixtures')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    exe = args.cortex.resolve(strict=True)
    root = args.fixtures.resolve(strict=True)
    before = fixture_hashes(root)
    passed: list[str] = []
    failures: list[str] = []

    def check(ok: bool, label: str) -> None:
        (passed if ok else failures).append(label)
        print(('PASS ' if ok else 'FAIL ') + label, flush=True)

    def cli(relative: str, kind: str, child: str = '') -> None:
        command = [str(exe), 'inspect', str(root / relative)]
        if child:
            command += ['--child', child]
        run = subprocess.run(command, cwd=exe.parent, capture_output=True, text=True,
                             encoding='utf-8', timeout=60)
        report = json.loads(run.stdout)
        check(run.returncode == 0 and report.get('ok') is True and report.get('format') == kind,
              'CLI ' + relative + ('!' + child if child else ''))
        if not child:
            check(report.get('identity', {}).get('sha256') == before[relative], 'CLI SHA256 ' + relative)

    for relative, kind in CASES:
        cli(relative, kind)
    cli('archive-next/package.tgz', 'npm-tar', 'payload')
    cli('package-next/sample3.crx', 'webextension', 'archive.zip')
    cli('certificates/bundle.pem', 'x509', 'certificate-0.der')
    cli('package-next/sample.ear', 'war', 'sample.war')

    absent = Mcp(exe, None)
    try:
        absent.initialize()
        failed, result = absent.call('cortex_artifact_inspect', {'path': str(root / CASES[0][0])})
        check(failed and result.get('error') == 'artifact_roots_not_configured', 'MCP root is mandatory')
    finally:
        absent.close()

    server = Mcp(exe, root)
    try:
        server.initialize()
        tools = {t['name']: t for t in server.request('tools/list', {}).get('tools', [])}
        for name in ('cortex_artifact_inspect', 'cortex_artifact_children', 'cortex_artifact_read'):
            check(tools.get(name, {}).get('_cortex', {}).get('read_only') is True,
                  name + ' read-only classification unchanged')
        for relative, kind in CASES:
            failed, result = server.call('cortex_artifact_inspect', {'path': str(root / relative), 'max_entries': 8})
            check(not failed and result.get('ok') is True and result.get('format') == kind, 'MCP ' + relative)
            check(result.get('identity', {}).get('sha256') == before[relative], 'MCP SHA256 ' + relative)

        def inspect(relative: str, child: str, kind: str) -> dict:
            failed, result = server.call('cortex_artifact_inspect', {'path': str(root / relative), 'child': child})
            check(not failed and result.get('format') == kind, 'MCP nested ' + relative + '!' + child)
            return result

        def read(relative: str, child: str, expected: bytes) -> None:
            failed, result = server.call('cortex_artifact_read',
                {'path': str(root / relative), 'child': child, 'length': len(expected)})
            check(not failed and result.get('hex') == expected.hex(), 'MCP exact bytes ' + relative + '!' + child)

        inspect('archive-next/package.tgz', 'payload', 'npm-tar')
        inspect('archive-next/sample.deb', 'data.tar.gz', 'gzip')
        inspect('archive-next/sample.deb', 'data.tar.gz!payload', 'tar')
        read('archive-next/sample.deb', 'data.tar.gz!payload!usr/share/cortex/hello.txt',
             (root / 'archive-next/expected.bin').read_bytes())
        inspect('package-next/sample3.crx', 'archive.zip', 'webextension')
        read('package-next/sample3.crx', 'archive.zip!worker.js',
             b'throw new Error("Cortex parser fixture: never execute");\n')
        inspect('certificates/bundle.pem', 'certificate-0.der', 'x509')
        inspect('certificates/signed.p7m', 'certificate-0.der', 'x509')
        read('certificates/signed.p7m', 'content.der', (root / 'certificates/content.bin').read_bytes())
        inspect('package-next/sample.ear', 'sample.war', 'war')
        read('package-next/jdk.jmod', 'archive.zip!classes/module-info.class', bytes.fromhex('cafebabe'))

        for relative, child in [('archive-next/unsafe.tar', '../escape.bin'),
                                ('archive-next/unsafe.tar', 'link'),
                                ('archive-next/duplicate.tar', 'same'),
                                ('archive-next/unsafe.a', '../escape')]:
            failed, result = server.call('cortex_artifact_read', {'path': str(root / relative), 'child': child, 'length': 4})
            check(failed, 'MCP unsafe/ambiguous child refused ' + relative + '!' + child)
        for relative in ('archive-next/bad-crc.gz', 'archive-next/thin.a',
                         'package-next/oversized.crx', 'certificates/bad-base64.pem'):
            failed, result = server.call('cortex_artifact_inspect', {'path': str(root / relative)})
            check(failed or result.get('ok') is False, 'MCP malformed/unsupported input refused ' + relative)
        failed, result = server.call('cortex_artifact_inspect', {'path': str(root.parent.parent / 'README.md')})
        check(failed and result.get('error') == 'path_outside_artifact_roots', 'MCP outside-root denied')
        for relative in ('certificates/certificate.der', 'certificates/signed.p7m', 'package-next/sample3.crx'):
            failed, result = server.call('cortex_artifact_inspect', {'path': str(root / relative)})
            check(not failed and result.get('data', {}).get('signature_verified') is False,
                  'MCP does not invent signature verification ' + relative)
        failed, result = server.call('cortex_artifact_inspect', {'path': str(root / 'package-next/RELEASES'), 'max_entries': 1})
        check(not failed and len(result.get('data', {}).get('packages', [])) == 1 and result['data']['truncated'] is True,
              'MCP package output limit applied')
    finally:
        server.close()
    check(before == fixture_hashes(root), 'all analyzed fixture files unchanged')
    report = {'passed': len(passed), 'failed': failures, 'writes_authorized': False,
              'fixture_executions': 0, 'fixture_hash_count': len(before), 'cases': len(CASES)}
    print(json.dumps(report), flush=True)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
