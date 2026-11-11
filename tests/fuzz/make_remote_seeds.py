#!/usr/bin/env python3
"""Valid protocol/request/response seeds. No credentials or live processes."""
from pathlib import Path
import json
import struct
import sys


def main():
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    identity = {'pid': 42, 'target_id': 'b'*32 + ':42:fixture-1'}
    capabilities = {'processes': True, 'maps': True, 'memory_read': True, 'memory_write': False,
                    'target_generation': True, 'breakpoints': False, 'hardware_tested': False,
                    'read_effect': 'fixture_only', 'max_memory_bytes': 65536,
                    'max_write_bytes': 32768, 'memory_scan': True, 'map_pagination': True}
    rows = [
        ('info', {}, {'ok': True, 'capabilities': capabilities}),
        ('processes', {'limit': 2}, {'ok': True, 'processes': [], 'total': 0, 'truncated': False}),
        ('maps', dict(identity, limit=1, offset=1, expected_revision='c'*64),
         dict(identity, ok=True, maps=[], total=1, offset=1, next_offset=None, map_revision='c'*64, truncated=False)),
        ('read', dict(identity, address='0x1000', length=4), dict(identity, ok=True, address='0x1000', length=4, hex='00010203')),
        ('write', dict(identity, address='0x1000', hex='aabb', expected_hex='0001', expected_name='fixture', mutation_permission=True),
         dict(identity, ok=True, address='0x1000', length=2, write_state='readback_matched', atomic=False)),
        ('scan', dict(identity, address='0x1000', length=4, pattern='00 ?? 02', max_matches=2),
         dict(identity, ok=True, address='0x1000', length=4, pattern='00 ?? 02', matches=[{'address':'0x1000','offset':0}], total_matches=1, truncated=False, scan_scope='single_bounded_read', live_snapshot_atomic=False)),
    ]
    for action, args, result in rows:
        value = {'schema':'cortex.remote.request.v1','session':'a'*32,'instance':'b'*32,'action':action,'arguments':args,'result':result}
        body = json.dumps(value, separators=(',', ':')).encode()
        header = struct.pack('!4sBBHII', b'CRTX', 1, 2, 0, 1, len(body))
        (folder/action).write_bytes(header + body)
    (folder/'pattern').write_bytes(b'48 8b ?? 00')
    print('Wrote', len(rows)+1, 'public, deterministic protocol seeds')


if __name__ == '__main__':
    main()
