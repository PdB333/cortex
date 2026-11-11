#!/usr/bin/env python3
"""Pins, bounded queries and hostile authenticated replies over actual TLS.

Only our agent's explicit fixture backend is used. All traffic stays on
loopback; no console, unrelated process or analyzed file is executed.
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import struct
import threading
import uuid
from remote_agent_e2e import Agent, RemoteMcp, create, port, receive, send, save, BASE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True, type=Path)
    parser.add_argument('--agent', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--cortex', action='store_true')
    parser.add_argument('--report', type=Path)
    options = parser.parse_args()
    client, agent = options.client.resolve(strict=True), options.agent.resolve(strict=True)
    work = options.work.resolve() / uuid.uuid4().hex
    endpoint = '127.0.0.1:' + str(port())
    create(work, endpoint, platform='fixture', days=2)
    cpath, apath = work / 'client/client.json', work / 'agent/agent.json'
    cconfig, aconfig = json.loads(cpath.read_text()), json.loads(apath.read_text())
    cconfig['allow_writes'] = True
    aconfig['allow_writes'] = True
    save(cpath, cconfig); save(apath, aconfig)
    passed, failed = [], []

    def check(ok, name):
        (passed if ok else failed).append(name)
        print(('PASS ' if ok else 'FAIL ') + name, flush=True)

    def cli(action, extra=(), config=cpath):
        command = [str(client)] + (['ps5'] if options.cortex else []) + [action, '--config', str(config)] + list(extra)
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=20)
        if not result.stdout.strip():
            raise RuntimeError(f'client produced no result, exit={result.returncode}: {result.stderr}')
        return result.returncode, json.loads(result.stdout)

    def config_file(name, **values):
        path = work / 'client' / name
        save(path, dict(cconfig, **values))
        return path

    def connect(certificate=None, key=None):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.load_verify_locations(str(work / 'client/ca.crt'))
        context.load_cert_chain(str(certificate or work / 'client/identity.crt'), str(key or work / 'client/identity.key'))
        raw = socket.create_connection(('127.0.0.1', int(endpoint.split(':')[1])), timeout=5)
        return context.wrap_socket(raw, server_hostname='cortex-agent.local')

    def exchange(action, arguments):
        with connect() as peer:
            hello = receive(peer, 1, 0)
            send(peer, 2, 1, {'schema': 'cortex.remote.request.v1', 'session': hello['session'],
                            'instance': hello['instance'], 'action': action, 'arguments': arguments})
            return receive(peer, 3, 1)['result']

    running = Agent(agent, apath)
    try:
        code, info = cli('info')
        check(code == 0 and info['capabilities']['memory_scan'] and info['capabilities']['map_pagination'], 'new query capabilities advertised')
        check(info['capabilities']['max_write_bytes'] == 32768, 'write capability fits expected/replacement frame')
        check(info['peer_certificate_sha256'] == cconfig['server_certificate_sha256'], 'generated server pin enforced')
        der = ssl.PEM_cert_to_DER_cert((work / 'client/identity.crt').read_text())
        check(aconfig['allowed_client_sha256'] == [hashlib.sha256(der).hexdigest()], 'generated client pin allowlist')
        check((work / '.gitignore').read_text() == '*\n', 'generated secrets ignored')
        _, processes = cli('processes')
        target = processes['processes'][0]['target_id']
        identity = ['--pid', '42', '--target-id', target]
        scan = identity + ['--address', hex(BASE), '--length', '1024', '--pattern', '00', '--max-matches', '2']
        code, result = cli('scan', scan)
        check(code == 0 and result['total_matches'] == 4 and result['truncated'] and [r['offset'] for r in result['matches']] == [0, 256], 'scan exact total and bounded matches')
        check(result.get('live_snapshot_atomic') is False and result['scan_scope'] == 'single_bounded_read', 'scan is not a claimed live atomic snapshot')
        code, result = cli('scan', identity + ['--address', hex(BASE), '--length', '512', '--pattern', 'FE ff ?? 01'])
        check(code == 0 and result['pattern'] == 'fe ff ?? 01' and result['matches'] == [{'address': hex(BASE + 254), 'offset': 254}], 'wildcard scan across byte-pattern boundary')
        code, result = cli('scan', identity + ['--address', hex(BASE), '--length', '1', '--pattern', '00 01'])
        check(code == 0 and result['total_matches'] == 0, 'pattern longer than read')
        for pattern in ['?? ??', '?a', 'abc', 'gg']:
            code, result = cli('scan', identity + ['--address', hex(BASE), '--length', '16', '--pattern', pattern])
            check(code != 0 and result.get('stage') == 'validate', 'invalid pattern before network: ' + pattern)
        code, result = cli('scan', identity + ['--address', hex(BASE), '--length', '65537', '--pattern', '00'])
        check(code != 0 and result.get('stage') == 'validate', 'scan range quota before network')
        code, result = cli('scan', ['--pid', '42', '--target-id', 'stale', '--address', hex(BASE), '--length', '16', '--pattern', '00'])
        check(code != 0 and result.get('error') == 'stale_target', 'scan rejects stale process identity')
        code, first = cli('maps', identity + ['--limit', '1'])
        check(code == 0 and first['total'] == 4 and first['next_offset'] == 1 and len(first['maps']) == 1, 'first real-agent map page')
        revision = first['map_revision']
        starts = [first['maps'][0]['start']]
        for offset in (1, 2, 3):
            code, page = cli('maps', identity + ['--limit', '1', '--offset', str(offset), '--expected-revision', revision])
            check(code == 0 and page['offset'] == offset and page['map_revision'] == revision, f'continuation page {offset}')
            starts.extend(row['start'] for row in page['maps'])
        check(len(set(starts)) == 4 and page['next_offset'] is None, 'all pages disjoint and terminal cursor explicit')
        # Independent SHA-256 reference for the canonical map revision encoding.
        digest = hashlib.sha256()
        def number(value): digest.update(struct.pack('!Q', value))
        def text(value): encoded=value.encode(); number(len(encoded)); digest.update(encoded)
        text('cortex.remote.map.v1'); text(target); number(4)
        for index in range(4):
            number(BASE+index*262144); number(BASE+(index+1)*262144); number(index*262144); number(3); text('fixture.data.'+str(index))
        check(revision == digest.hexdigest(), 'map revision checked against independent hashlib oracle')
        code, result = cli('maps', identity + ['--offset', '1'])
        check(code != 0 and result['error'] == 'map_revision_required', 'continuation requires matching revision')
        code, result = cli('maps', identity + ['--offset', '1', '--expected-revision', '0' * 64])
        check(code != 0 and result['error'] == 'map_revision_changed', 'map mismatch refuses mixed layouts')
        code, result = cli('maps', identity + ['--offset', '5', '--expected-revision', revision])
        check(code != 0 and result['error'] == 'page_offset_out_of_range', 'map offset bounded by actual map')

        # Full-size write goes through the protocol, not OS command-line limits.
        # It touches only the fixture vector, then restores it before other tests.
        before = bytes(range(256)) * 128
        mutation = {'pid': 42, 'target_id': target, 'address': hex(BASE), 'expected_name': 'CortexFixture',
                    'expected_hex': before.hex(), 'hex': (b'\xaa' * 32768).hex(), 'mutation_permission': True}
        result = exchange('write', mutation)
        check(result.get('ok') and result.get('length') == 32768 and result.get('write_state') == 'readback_matched', 'maximum 32KiB write fits and executes on fixture')
        code, data = cli('read', identity + ['--address', hex(BASE), '--length', '65536'])
        check(code == 0 and data['hex'][:65536] == 'aa' * 32768, 'large write independently observed')
        mutation['hex'], mutation['expected_hex'] = mutation['expected_hex'], mutation['hex']
        check(exchange('write', mutation).get('ok'), 'large fixture mutation restored')
        mutation['hex'] = 'aa' * 32769; mutation['expected_hex'] = '00' * 32769
        result = exchange('write', mutation)
        check(not result['ok'] and result['write_state'] == 'not_written', 'oversized write rejected before dispatch')

        wrong = config_file('wrong-pin.json', server_certificate_sha256='0' * 64)
        code, result = cli('info', config=wrong)
        check(code != 0 and result['error'] == 'server_certificate_not_pinned' and result['stage'] == 'peer_identity', 'trusted CA alone cannot bypass server pin')
        wrong = config_file('malformed-pin.json', server_certificate_sha256='z' * 64)
        code, result = cli('info', config=wrong)
        check(code != 0 and result['stage'] == 'validate', 'pin syntax checked before connection')

        # A second legitimate client of the same CA is not the approved peer.
        alternate = work / 'alternate'; alternate.mkdir()
        env = dict(os.environ, OPENSSL_CONF=str(work / 'authority/provision.cnf'))
        openssl = os.environ.get('CORTEX_OPENSSL') or shutil.which('openssl')
        ext = alternate / 'client.ext'
        ext.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n')
        for arguments in [
            ['req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', str(alternate/'key.pem'), '-out', str(alternate/'csr.pem'), '-subj', '/CN=other-client'],
            ['x509', '-req', '-in', str(alternate/'csr.pem'), '-CA', str(work/'authority/ca.crt'), '-CAkey', str(work/'authority/ca.key'), '-CAcreateserial', '-out', str(alternate/'cert.pem'), '-days', '1', '-extfile', str(ext)],
        ]:
            subprocess.run([openssl, *arguments], check=True, capture_output=True, env=env, timeout=15)
        rejected = False
        try:
            with connect(alternate/'cert.pem', alternate/'key.pem') as peer:
                receive(peer, 1, 0)
        except (EOFError, OSError):
            rejected = True
        check(rejected, 'CA-valid but non-allowlisted client denied before hello')
        check(cli('info')[0] == 0, 'authorized client survives rejected peer')

        if options.cortex:
            mcp = RemoteMcp(client, cpath)
            try:
                mcp.initialize()
                tools = {row['name']: row for row in mcp.request('tools/list', {})['tools']}
                check('cortex_ps5_scan' in tools and tools['cortex_ps5_scan']['_cortex']['read_only'], 'MCP bounded scan registered as read-only')
                check(tools['cortex_ps5_write']['inputSchema']['properties']['hex']['maxLength'] == 65536, 'MCP write schema matches wire cap')
                params = {'pid': 42, 'target_id': target, 'address': hex(BASE), 'length': 1024, 'pattern': '00', 'max_matches': 2}
                error, result = mcp.call('cortex_ps5_scan', params)
                check(not error and result['total_matches'] == 4, 'MCP scan executes on own agent')
                params['mutation_permission'] = True
                error, result = mcp.call('cortex_ps5_scan', params)
                check(error, 'MCP scan cannot add mutation authority')
                error, result = mcp.call('cortex_ps5_maps', {'pid': 42, 'target_id': target, 'offset': 1, 'limit': 1, 'expected_revision': revision})
                check(not error and result['offset'] == 1, 'MCP map continuation')
            finally:
                mcp.close()
    finally:
        running.close()

    # The client must distrust even an authenticated server's application data.
    # This peer uses the real agent identity but constructs replies independently.
    caps = info['capabilities']
    instance = 'b' * 32
    fake_token = instance + ':42:fixture-1'
    fake_identity = ['--pid', '42', '--target-id', fake_token]
    good_map = {'ok': True, 'pid': 42, 'target_id': fake_token, 'maps': [
        {'name': 'memory', 'start': hex(BASE), 'end': hex(BASE+4096), 'offset': '0x0', 'read': True, 'write': False, 'execute': False}],
        'offset': 0, 'total': 1, 'next_offset': None, 'map_revision': 'c'*64, 'truncated': False}
    bad_map = copy.deepcopy(good_map); del bad_map['maps'][0]['read']
    duplicate = {'ok': True, 'processes': [{'pid': 42, 'name': 'one', 'target_id': fake_token}, {'pid': 42, 'name': 'two', 'target_id': fake_token}], 'total': 2, 'truncated': False}
    bad_scan = {'ok': True, 'pid': 42, 'target_id': fake_token, 'address': hex(BASE), 'length': 4,
                'pattern': '00', 'matches': [{'address': hex(BASE+10), 'offset': 10}], 'total_matches': 1,
                'truncated': False, 'scan_scope': 'single_bounded_read', 'live_snapshot_atomic': False}
    bad_write = {'ok': True, 'pid': 42, 'target_id': fake_token, 'address': hex(BASE), 'length': 1, 'write_state': 'readback_matched', 'atomic': True}
    examples = [
        ('missing map permission', 'maps', fake_identity, bad_map, 'response_boolean_required'),
        ('duplicate process identity', 'processes', [], duplicate, 'duplicate_process_identity'),
        ('out-of-window scan result', 'scan', fake_identity+['--address',hex(BASE),'--length','4','--pattern','00'], bad_scan, 'integer_out_of_range'),
        ('false atomic-write claim', 'write', fake_identity+['--address',hex(BASE),'--hex','aa','--expected-hex','00','--expected-name','one','--allow-writes','--confirm-write'], bad_write, 'invalid_write_state'),
    ]
    for label, action, extra, answer, expected_error in examples:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0)); listener.listen(1); listener.settimeout(5)
            profile = config_file('hostile.json', endpoint='127.0.0.1:'+str(listener.getsockname()[1]))
            observed = []
            def peer_server():
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
                context.verify_mode = ssl.CERT_REQUIRED
                context.load_verify_locations(str(work/'agent/ca.crt'))
                context.load_cert_chain(str(work/'agent/identity.crt'),str(work/'agent/identity.key'))
                try:
                    raw, _ = listener.accept()
                    with context.wrap_socket(raw, server_side=True) as peer:
                        peer.settimeout(5)
                        hello = {'schema':'cortex.remote.hello.v1','protocol':'cortex-remote','version':1,
                                 'instance':instance,'session':'a'*32,'platform':'fixture','capabilities':caps}
                        send(peer,1,0,hello)
                        observed.append(receive(peer,2,1))
                        send(peer,3,1,{'schema':'cortex.remote.response.v1','session':'a'*32,'instance':instance,'result':answer})
                except Exception as error:
                    observed.append(type(error).__name__)
            thread = threading.Thread(target=peer_server, daemon=True); thread.start()
            code, result = cli(action, extra, profile)
            thread.join(timeout=6)
            check(len(observed)==1 and isinstance(observed[0],dict) and code!=0 and result.get('error')==expected_error, 'hostile authenticated response: '+label)
            if action == 'write':
                check(result.get('write_state')=='may_have_changed' and result.get('retry_safe') is False, 'invalid write reply never becomes safe-to-retry')

    # Provisioning failures leave an ignore guard, not untracked private keys.
    original = os.environ.get('CORTEX_OPENSSL')
    faildir = work / 'failed-provision'
    os.environ['CORTEX_OPENSSL'] = str(work/'does-not-exist')
    try:
        try:
            create(faildir, endpoint, platform='fixture')
        except OSError:
            pass
        check((faildir/'.gitignore').read_text()=='*\n', 'failed provisioning still protects generated secrets')
    finally:
        if original is None: os.environ.pop('CORTEX_OPENSSL',None)
        else: os.environ['CORTEX_OPENSSL']=original
    for bad_endpoint in ['0.0.0.0:744','224.0.0.1:744','127.0.00.1:744']:
        rejected=False
        try: create(work/'invalid',bad_endpoint,platform='fixture')
        except ValueError: rejected=True
        check(rejected and not (work/'invalid').exists(),'provision endpoint validation '+bad_endpoint)

    report = {'passed': len(passed), 'failed': failed, 'network_scope': 'loopback only',
              'hardware_tested': False, 'backend': 'fixture', 'cortex_mcp_tested': options.cortex}
    print(json.dumps(report), flush=True)
    if options.report: options.report.parent.mkdir(parents=True,exist_ok=True); save(options.report,report)
    return 1 if failed else 0

if __name__ == '__main__':
    raise SystemExit(main())
