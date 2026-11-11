#!/usr/bin/env python3
"""Read-only wave-1 CLI/MCP integration against owned fixture files.

Only cortex.exe itself is started. No fixture, installer, COM component,
script or target process is run. There is no --allow-writes.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading
import time


class Mcp:
    def __init__(self, executable: Path, root: Path | None):
        command = [str(executable), 'mcp', '--tools', 'all']
        if root is not None:
            command += ['--artifact-root', str(root)]
        self.proc = subprocess.Popen(command, cwd=executable.parent, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding='utf-8', errors='strict', bufsize=1)
        self.messages: queue.Queue = queue.Queue(maxsize=128)
        self.stderr = collections.deque(maxlen=100)
        self.next_id = 0
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        threading.Thread(target=self._errors, daemon=True).start()

    def _errors(self):
        try:
            for line in self.proc.stderr:
                self.stderr.append(line[:4096])
        except (OSError, ValueError):
            pass

    def _read(self):
        try:
            for line in self.proc.stdout:
                if len(line) > 16 * 1024 * 1024:
                    raise RuntimeError('MCP reply exceeds test limit')
                self.messages.put(json.loads(line), timeout=5)
        except Exception as error:
            self.messages.put(error, timeout=5)
        finally:
            try:
                self.messages.put(None, timeout=5)
            except queue.Full:
                pass

    def request(self, method: str, params: dict | None = None, timeout: float = 45):
        self.next_id += 1
        message = {'jsonrpc': '2.0', 'id': self.next_id, 'method': method}
        if params is not None:
            message['params'] = params
        self.proc.stdin.write(json.dumps(message) + '\n')
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('MCP request timed out: ' + method)
            response = self.messages.get(timeout=remaining)
            if isinstance(response, Exception):
                raise response
            if response is None:
                raise RuntimeError('MCP closed: ' + ''.join(self.stderr))
            if response.get('id') == self.next_id:
                if 'error' in response:
                    raise RuntimeError(str(response['error']))
                return response['result']

    def initialize(self):
        result = self.request('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                              'clientInfo': {'name': 'formats-wave1-e2e', 'version': '1'}})
        self.proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
        self.proc.stdin.flush()
        return result

    def call(self, name: str, arguments: dict):
        result = self.request('tools/call', {'name': name, 'arguments': arguments})
        data = result.get('structuredContent')
        if data is None:
            texts = [x['text'] for x in result.get('content', []) if x.get('type') == 'text']
            data = json.loads(texts[0]) if texts else result
        return bool(result.get('isError')), data

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            self.proc.kill()
            self.proc.wait(timeout=10)
        self.reader.join(timeout=2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cortex', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, default=Path(__file__).resolve().parent / 'fixtures')
    args = parser.parse_args()
    executable, root = args.cortex.resolve(strict=True), args.fixtures.resolve(strict=True)
    failures, passed = [], []

    def check(condition, message):
        (passed if condition else failures).append(message)
        print(('PASS ' if condition else 'FAIL ') + message, flush=True)

    cases = [('pdb/sample-x64.pdb', 'pdb'), ('elf/actor.elf', 'elf'),
             ('cab/mszip-history.cab', 'cab'), ('compound/structured-v4.cfb', 'cfb'),
             ('compound/microsoft.msi', 'msi'), ('appx/sample.msix', 'appx'),
             ('appx/sample.msixbundle', 'appxbundle')]
    for relative, kind in cases:
        target = root / relative
        run = subprocess.run([str(executable), 'inspect', str(target)], cwd=executable.parent,
                             capture_output=True, text=True, encoding='utf-8', timeout=60)
        report = json.loads(run.stdout)
        check(run.returncode == 0 and report.get('format') == kind, 'CLI ' + relative)
        check(report.get('identity', {}).get('sha256') == hashlib.sha256(target.read_bytes()).hexdigest(),
              'CLI SHA256 ' + relative)

    server = Mcp(executable, None)
    try:
        server.initialize()
        failed, result = server.call('cortex_artifact_inspect', {'path': str(root / 'pdb/sample-x64.pdb')})
        check(failed and result.get('error') == 'artifact_roots_not_configured', 'MCP refuses absent artifact root')
    finally:
        server.close()

    server = Mcp(executable, root)
    try:
        server.initialize()
        tools = {tool['name']: tool for tool in server.request('tools/list', {}).get('tools', [])}
        for name in ('cortex_artifact_inspect', 'cortex_artifact_children', 'cortex_artifact_read'):
            check(tools.get(name, {}).get('_cortex', {}).get('read_only') is True, name + ' remains read-only')
        for relative, kind in cases:
            target = root / relative
            failed, result = server.call('cortex_artifact_inspect', {'path': str(target), 'max_entries': 8})
            check(not failed and result.get('format') == kind, 'MCP ' + relative)
            check(result.get('identity', {}).get('sha256') == hashlib.sha256(target.read_bytes()).hexdigest(),
                  'MCP SHA256 ' + relative)
        msi = str(root / 'compound/microsoft.msi')
        failed, result = server.call('cortex_artifact_children', {'path': msi})
        check(not failed and 'streams/payload.cab' in {x['name'] for x in result.get('children', [])},
              'MCP exposes embedded CAB')
        failed, result = server.call('cortex_artifact_inspect', {'path': msi, 'child': 'streams/payload.cab'})
        check(not failed and result.get('format') == 'cab', 'MCP MSI -> CAB provider')
        failed, result = server.call('cortex_artifact_read',
                                     {'path': msi, 'child': 'streams/payload.cab!a.bin', 'length': 32})
        check(not failed and result.get('hex') == (root / 'cab/expected.bin').read_bytes()[:32].hex(),
              'MCP MSI -> CAB -> original file bytes')
        failed, result = server.call('cortex_artifact_read',
                                     {'path': str(root / 'cab/unsafe.cab'), 'child': '../escape.bin', 'length': 4})
        check(failed, 'MCP rejects unsafe CAB name')
        failed, result = server.call('cortex_artifact_inspect', {'path': str(root.parent.parent / 'README.md')})
        check(failed and result.get('error') == 'path_outside_artifact_roots', 'MCP outside-root file denied')
        failed, result = server.call('cortex_artifact_inspect', {'path': str(root / 'appx/tampered.msix')})
        check(not failed and result.get('data', {}).get('appx', {}).get('block_map', {}).get('failed_files') == 1,
              'MCP reports APPX block mismatch without claiming package authenticity')
    finally:
        server.close()
    print(json.dumps({'passed': len(passed), 'failed': failures, 'writes_authorized': False,
                      'fixture_executions': 0}))
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
