#!/usr/bin/env python3
"""Cortex client <-> Cortex agent tests, plus an independent Python TLS/wire peer.

All network traffic stays on 127.0.0.1. The native agent is started with its
explicit --fixture backend, never identified as PS5. Certificates/keys are
freshly generated in the ignored work directory, never checked into source.
"""
from __future__ import annotations
import argparse
import collections
import json
import os
from pathlib import Path
import queue
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import uuid
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'remote'))
from create_credentials import create
from formats_wave1_e2e import Mcp

HEADER = struct.Struct('!4sBBHII')
BASE = 0x100000


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def exact(sock, size):
    out = bytearray()
    while len(out) < size:
        chunk = sock.recv(size - len(out))
        if not chunk:
            raise EOFError('peer closed')
        out.extend(chunk)
    return bytes(out)


def receive(sock, kind, number):
    magic, version, got, flags, seq, size = HEADER.unpack(exact(sock, 16))
    assert (magic, version, got, flags, seq) == (b'CRTX', 1, kind, 0, number)
    assert 0 < size <= 262144
    return json.loads(exact(sock, size))


def send(sock, kind, number, data, fragmented=False):
    body = json.dumps(data, separators=(',', ':')).encode()
    raw = HEADER.pack(b'CRTX', 1, kind, 0, number, len(body)) + body
    if fragmented:
        for at in range(0, len(raw), 3):
            sock.sendall(raw[at:at + 3])
    else:
        sock.sendall(raw)


def save(path, data):
    path.write_text(json.dumps(data, indent=2), encoding='utf-8')


class Agent:
    def __init__(self, exe, config):
        self.stderr = collections.deque(maxlen=100)
        self.proc = subprocess.Popen([str(exe), '--config', str(config), '--fixture'], stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        lines = queue.Queue()
        threading.Thread(target=lambda: lines.put(self.proc.stdout.readline()), daemon=True).start()
        threading.Thread(target=self.errors, daemon=True).start()
        try:
            line = lines.get(timeout=10)
            self.ready = json.loads(line)
            assert self.ready['ready'] and self.ready['platform'] == 'fixture'
        except Exception:
            self.close()
            raise RuntimeError('agent did not start: ' + '\n'.join(self.stderr))

    def errors(self):
        for line in self.proc.stderr:
            self.stderr.append(line.rstrip())

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        for stream in (self.proc.stdout, self.proc.stderr):
            if stream:
                stream.close()


class RemoteMcp(Mcp):
    def __init__(self, executable, config='', writes=False):
        command = [str(executable), 'mcp', '--tools', 'all']
        if config:
            command += ['--ps5-config', str(config)]
        if writes:
            command += ['--allow-writes']
        self.proc = subprocess.Popen(command, cwd=executable.parent, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='strict', bufsize=1)
        self.messages = queue.Queue(maxsize=128)
        self.stderr = collections.deque(maxlen=100)
        self.next_id = 0
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        threading.Thread(target=self._errors, daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True, type=Path)
    parser.add_argument('--agent', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--cortex', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    client, agent_exe = args.client.resolve(strict=True), args.agent.resolve(strict=True)
    work = args.work.resolve() / uuid.uuid4().hex
    endpoint = '127.0.0.1:' + str(port())
    create(work, endpoint, platform='fixture', days=2)
    cpath, apath = work/'client/client.json', work/'agent/agent.json'
    cconfig, aconfig = json.loads(cpath.read_text()), json.loads(apath.read_text())
    cconfig['allow_writes'] = True
    aconfig['allow_writes'] = True
    aconfig['timeout_ms'] = 1000
    save(cpath, cconfig)
    save(apath, aconfig)
    checks, failures = [], []

    def check(ok, name):
        checks.append(name)
        if not ok:
            failures.append(name)
        print(('PASS ' if ok else 'FAIL ') + name, flush=True)

    def cli(action, extra=(), config=cpath):
        command = [str(client)] + (['ps5'] if args.cortex else []) + [action]
        if config:
            command += ['--config', str(config)]
        result = subprocess.run(command + list(extra), capture_output=True, text=True, encoding='utf-8', errors='strict', timeout=20)
        if not result.stdout.strip():
            raise RuntimeError(f'client exit {result.returncode}: {result.stderr}')
        return result.returncode, json.loads(result.stdout)

    def new_config(name, **changes):
        path = work/'client'/name
        data = dict(cconfig, **changes)
        save(path, data)
        return path

    def tls_client(client_cert=True):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_verify_locations(str(work/'client/ca.crt'))
        if client_cert:
            ctx.load_cert_chain(str(work/'client/identity.crt'), str(work/'client/identity.key'))
        return ctx

    def connect(client_cert=True):
        return tls_client(client_cert).wrap_socket(socket.create_connection(('127.0.0.1', int(endpoint.rsplit(':', 1)[1])), timeout=4), server_hostname='cortex-agent.local')

    def request(hello, action='info', arguments=None):
        return {'schema':'cortex.remote.request.v1', 'session':hello['session'], 'instance':hello['instance'],
                'action':action, 'arguments':{} if arguments is None else arguments}

    running = Agent(agent_exe, apath)
    try:
        code, result = cli('info', config=None)
        check(code != 0 and result.get('error') == 'remote_config_not_configured', 'operator config required')
        code, result = cli('info')
        check(code == 0 and result.get('protocol') == 'cortex-remote' and result.get('platform') == 'fixture', 'real Cortex client and agent handshake')
        check(len(result.get('peer_certificate_sha256', '')) == 64, 'authenticated peer fingerprint')
        check(result.get('capabilities', {}).get('hardware_tested') is False, 'fixture does not claim console validation')
        code, processes = cli('processes')
        target = processes['processes'][0]
        check(code == 0 and target['pid'] == 42 and target['name'] == 'CortexFixture', 'native agent enumerates its own test backend')
        identity = ['--pid', '42', '--target-id', target['target_id']]
        code, maps = cli('maps', identity)
        check(code == 0 and maps['maps'][0]['start'] == hex(BASE), 'generation-bound memory maps')
        read = identity + ['--address', hex(BASE), '--length', '4']
        code, data = cli('read', read)
        check(code == 0 and data['hex'] == '00010203', 'bounded read')
        code, data = cli('read', identity + ['--address', hex(BASE), '--length', '65536'])
        check(code == 0 and len(data['hex']) == 131072, 'maximum 64KiB read')
        for value, expected in [('65537','integer_out_of_range'),('0','integer_out_of_range'),('-1','invalid_number')]:
            code, data = cli('read', identity + ['--address', hex(BASE), '--length', value])
            check(code != 0 and data.get('error') == expected, 'read quota ' + value)
        code, data = cli('read', ['--pid','42','--target-id','stale','--address',hex(BASE),'--length','4'])
        check(code != 0 and data.get('error') == 'stale_target', 'stale target token rejected')
        write = identity + ['--address',hex(BASE),'--hex','aabbccdd','--expected-hex','00010203','--expected-name','CortexFixture']
        code, data = cli('write', write + ['--confirm-write'])
        check(code != 0 and data.get('error') == 'write_authority_required', 'operator write authority required')
        code, data = cli('write', write + ['--allow-writes'])
        check(code != 0 and data.get('error') == 'mutation_permission_required', 'per-call confirmation required')
        readonly = new_config('readonly.json', allow_writes=False)
        code, data = cli('write', write + ['--allow-writes','--confirm-write'], readonly)
        check(code != 0 and data.get('error') == 'client_config_read_only', 'client profile cannot be escalated by flag alone')
        wrong = list(write);wrong[wrong.index('00010203')] = 'ffffffff'
        code, data = cli('write', wrong + ['--allow-writes','--confirm-write'])
        check(code != 0 and data.get('error') == 'expected_bytes_mismatch' and data.get('write_state') == 'not_written', 'server compares expected bytes')
        wrong = list(write);wrong[wrong.index('CortexFixture')] = 'WrongProcess'
        code, data = cli('write', wrong + ['--allow-writes','--confirm-write'])
        check(code != 0 and data.get('error') == 'process_identity_mismatch', 'server checks process name')
        code, data = cli('write', write + ['--allow-writes','--confirm-write'])
        check(code == 0 and data.get('write_state') == 'readback_matched' and data.get('retry_safe') is False, 'native server write with readback and no automatic retry')
        code, data = cli('read', read)
        check(code == 0 and data['hex'] == 'aabbccdd', 'independent subsequent read sees write')
        wrongname = new_config('wrong-name.json', server_name='not-this-agent.local')
        code, data = cli('info', config=wrongname)
        check(code != 0 and 'tls_handshake_failed' in data.get('error',''), 'server certificate hostname enforced')
        platform = new_config('wrong-platform.json', expected_platform='ps5')
        code, data = cli('info', config=platform)
        check(code != 0 and data.get('error') == 'unexpected_remote_platform', 'no fixture masquerading as PS5')
        denied = False
        try:
            with connect(False) as peer:
                receive(peer, 1, 0)
        except (ssl.SSLError, ConnectionError, EOFError):
            denied = True
        check(denied, 'agent requires client certificate')
        # Independently issued credentials must fail in either direction.
        rogue=work/'rogue'
        create(rogue,endpoint,platform='fixture',days=2)
        untrusted=new_config('wrong-ca.json',ca_certificate=str(rogue/'client/ca.crt'))
        code,data=cli('info',config=untrusted)
        check(code != 0 and 'tls_handshake_failed' in data.get('error',''),'client refuses an untrusted server CA')
        unauthorized=new_config('rogue-client.json',certificate=str(rogue/'client/identity.crt'),private_key=str(rogue/'client/identity.key'))
        code,data=cli('info',config=unauthorized)
        check(code != 0 and 'tls_handshake_failed' in data.get('error',''),'agent refuses a client signed by a different CA')
        wrong_role=new_config('wrong-role.json',certificate=str(work/'agent/identity.crt'),private_key=str(work/'agent/identity.key'))
        code,data=cli('info',config=wrong_role)
        check(code != 0 and 'tls_handshake_failed' in data.get('error',''),'server-only certificate cannot impersonate a client')
        with connect() as peer:
            hello = receive(peer,1,0)
            send(peer,2,1,request(hello),fragmented=True)
            response = receive(peer,3,1)
            check(response['result']['ok'], 'independent Python TLS peer and fragmented request')
            old_request = request(hello)
            try:
                send(peer,2,1,old_request)
                receive(peer,3,1)
                rejected = False
            except (ConnectionError, EOFError, ssl.SSLError):
                rejected = True
            check(rejected, 'one request per connection prevents repeated sequence')
        for label, builder in [
            ('old-session replay',lambda hello: json.dumps(old_request).encode()),
            ('duplicate JSON keys',lambda hello: ('{"schema":"cortex.remote.request.v1","schema":"evil"}').encode()),
            ('deep JSON',lambda hello: ('{"x":'+ '['*33+'0'+']'*33+'}').encode()),
            ('private diagnostic',lambda hello: b'{"secret":"DO_NOT_LOG_MEMORY_BYTES","broken":}'),
        ]:
            rejected=False
            try:
                with connect() as peer:
                    hello=receive(peer,1,0);body=builder(hello)
                    peer.sendall(HEADER.pack(b'CRTX',1,2,0,1,len(body))+body)
                    receive(peer,3,1)
            except (ConnectionError, EOFError, ssl.SSLError):
                rejected=True
            check(rejected,label+' rejected')
        for label, raw in [('oversize frame',HEADER.pack(b'CRTX',1,2,0,1,262145)),('legacy protocol',HEADER.pack(b'OLD!',1,2,0,1,2)),('bad version',HEADER.pack(b'CRTX',2,2,0,1,2)),('bad sequence',HEADER.pack(b'CRTX',1,2,0,2,2))]:
            rejected=False
            try:
                with connect() as peer:
                    receive(peer,1,0);peer.sendall(raw+b'{}');receive(peer,3,1)
            except (ConnectionError,EOFError,ssl.SSLError):
                rejected=True
            check(rejected,label+' refused')
        with connect() as peer:
            hello=receive(peer,1,0)
            send(peer,2,1,request(hello,'info',{'endpoint':'127.0.0.1:1'}))
            result=receive(peer,3,1)['result']
            check(not result['ok'] and result['error']=='unknown_argument','server enforces strict tool arguments')
        with connect() as peer:
            receive(peer,1,0)
            rejected=False
            try:
                rejected=peer.recv(1)==b''
            except (ConnectionError,EOFError,ssl.SSLError):
                rejected=True
            check(rejected,'agent bounds idle authenticated connection time')
        check(not any('DO_NOT_LOG_MEMORY_BYTES' in line for line in running.stderr),'malformed input content is not leaked into audit logs')
        code, data = cli('info')
        check(code == 0 and data['ok'], 'agent survives malformed connections')
        # Reconnection gets a fresh nonce; an old instance invalidates all PIDs.
        old_token=target['target_id'];running.close();running=Agent(agent_exe,apath)
        code,data=cli('read',['--pid','42','--target-id',old_token,'--address',hex(BASE),'--length','4'])
        check(code != 0 and data.get('error')=='stale_target','agent restart invalidates old process identity')
        code,processes=cli('processes');target=processes['processes'][0]
        if args.cortex:
            server=RemoteMcp(client)
            try:
                server.initialize();err,result=server.call('cortex_ps5_info',{})
                check(err and result.get('error')=='remote_config_not_configured','MCP requires operator-selected config')
            finally:server.close()
            server=RemoteMcp(client,cpath)
            try:
                server.initialize();names={t['name']:t for t in server.request('tools/list',{}).get('tools',[])}
                check(all('cortex_ps5_'+n in names for n in ['info','processes','maps','read','write']),'MCP five tools registered')
                for action,values in [('info',{}),('processes',{}),('maps',{'pid':42,'target_id':target['target_id']}),('read',{'pid':42,'target_id':target['target_id'],'address':hex(BASE),'length':4})]:
                    err,result=server.call('cortex_ps5_'+action,values)
                    check(not err and result.get('ok'),'MCP dispatch '+action)
                err,result=server.call('cortex_ps5_info',{'config':str(cpath)})
                check(err,'MCP cannot change remote config')
                values={'pid':42,'target_id':target['target_id'],'address':hex(BASE),'hex':'aabbccdd','expected_hex':'00010203','expected_name':'CortexFixture','mutation_permission':True}
                err,result=server.call('cortex_ps5_write',values)
                check(err and result.get('error')=='write_authority_required','MCP cannot grant itself write authority')
            finally:server.close()
            server=RemoteMcp(client,cpath,True)
            try:
                server.initialize();err,result=server.call('cortex_ps5_write',values)
                check(not err and result.get('write_state')=='readback_matched','MCP explicit write reaches own agent')
            finally:server.close()
        # Agent-side policy cannot be overridden by either certificate or client.
        running.close();aconfig['allow_writes']=False;save(apath,aconfig);running=Agent(agent_exe,apath)
        _,p=cli('processes');token=p['processes'][0]['target_id']
        code,result=cli('write',['--pid','42','--target-id',token,'--address',hex(BASE),'--hex','aabbccdd','--expected-hex','00010203','--expected-name','CortexFixture','--allow-writes','--confirm-write'])
        check(code != 0 and result.get('error')=='agent_read_only','agent-side write policy enforced')
    finally:
        running.close()

    # A separate Python TLS server deliberately drops the write response.
    # The C++ client must not retry or claim the memory was unchanged.
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(5)
        fake_endpoint='127.0.0.1:'+str(listener.getsockname()[1])
        fake_config=new_config('uncertain.json',endpoint=fake_endpoint)
        observed=[]
        def fake_server():
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version=context.maximum_version=ssl.TLSVersion.TLSv1_2
            context.verify_mode=ssl.CERT_REQUIRED;context.load_verify_locations(str(work/'agent/ca.crt'))
            context.load_cert_chain(str(work/'agent/identity.crt'),str(work/'agent/identity.key'))
            try:
                conn,_=listener.accept()
                with context.wrap_socket(conn,server_side=True) as peer:
                    peer.settimeout(5)
                    hello={'schema':'cortex.remote.hello.v1','protocol':'cortex-remote','version':1,'session':'a'*32,'instance':'b'*32,'platform':'fixture','capabilities':{'memory_write':True}}
                    send(peer,1,0,hello);observed.append(receive(peer,2,1))
            except Exception as error:observed.append(str(error))
        thread=threading.Thread(target=fake_server,daemon=True);thread.start()
        code,result=cli('write',['--pid','42','--target-id','b'*32+':42:fixture-1','--address',hex(BASE),'--hex','aabbccdd','--expected-hex','00010203','--expected-name','CortexFixture','--allow-writes','--confirm-write'],fake_config)
        thread.join(timeout=6)
        check(len(observed)==1 and isinstance(observed[0],dict) and code != 0 and result.get('write_state')=='may_have_changed' and result.get('retry_safe') is False,'lost write response remains uncertain without retry')
    report={'passed':len(checks)-len(failures),'failed':failures,'network_scope':'loopback only','native_agent_tested':True,'backend':'fixture','hardware_tested':False,'cortex_mcp_tested':args.cortex}
    print(json.dumps(report),flush=True)
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True);save(args.report,report)
    return 1 if failures else 0


if __name__=='__main__':
    raise SystemExit(main())
