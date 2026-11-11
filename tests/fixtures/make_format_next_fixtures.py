#!/usr/bin/env python3
"""Build owned read-only parser fixtures using Python stdlib and OpenSSL.

No archive member, package script, certificate payload or type library is
executed. Private signing keys are created only in a temporary directory;
only public certificates/CMS objects and reference metadata are retained.
"""
from __future__ import annotations
import argparse
import base64
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import zlib

HERE = Path(__file__).resolve().parent
PAYLOAD = b'Cortex owned fixture\x00\xff\x01\n'


def tar_bytes(kind, items, pax=False):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w', format=kind) as archive:
        for name, data, type_ in items:
            item = tarfile.TarInfo(name)
            item.mtime = 0
            item.uid = item.gid = 0
            item.mode = 0o644
            item.type = type_
            if type_ == tarfile.SYMTYPE:
                item.linkname = '../external-target'
                item.size = 0
                archive.addfile(item)
            else:
                item.size = len(data)
                if pax:
                    item.pax_headers = {'comment': 'owned fixture', 'mtime': '0.25'}
                archive.addfile(item, io.BytesIO(data))
    return stream.getvalue()


def ar_bytes(items, bsd=False):
    result = bytearray(b'!<arch>\n')
    for name, data in items:
        raw = name.encode('utf-8')
        if bsd:
            member = ('#1/%d' % len(raw)).encode().ljust(16)
            data = raw + data
        else:
            if len(raw) > 15:
                raise ValueError(name)
            member = (raw + b'/').ljust(16)
        result += (member + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6)
                   + b'100644'.ljust(8) + str(len(data)).encode().ljust(10) + b'`\n')
        result += data
        if len(data) % 2:
            result += b'\n'
    return bytes(result)


def build_archives(folder):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'expected.bin').write_bytes(PAYLOAD)
    regular = [('alpha.bin', PAYLOAD, tarfile.REGTYPE), ('folder/note.txt', b'notes', tarfile.REGTYPE)]
    for name, mode, items in [
        ('ustar.tar', tarfile.USTAR_FORMAT, regular),
        ('v7.tar', tarfile.USTAR_FORMAT, regular),
        ('pax.tar', tarfile.PAX_FORMAT, [('deep/' + 'p' * 180 + '.txt', PAYLOAD, tarfile.REGTYPE)]),
        ('gnu.tar', tarfile.GNU_FORMAT, [('deep/' + 'g' * 180 + '.txt', PAYLOAD, tarfile.REGTYPE)]),
        ('unsafe.tar', tarfile.PAX_FORMAT, [('../escape.bin', PAYLOAD, tarfile.REGTYPE), ('link', b'', tarfile.SYMTYPE)]),
        ('duplicate.tar', tarfile.USTAR_FORMAT, [('same', b'1', tarfile.REGTYPE), ('same', b'2', tarfile.REGTYPE)]),
    ]:
        data = tar_bytes(mode, items, name == 'pax.tar')
        if name == 'v7.tar':
            data = bytearray(data)
            for off in (0, 1024):
                data[off+257:off+512] = bytes(255)
                data[off+148:off+156] = b'        '
                data[off+148:off+156] = ('%06o\x00 ' % sum(data[off:off+512])).encode('ascii')
            data = bytes(data)
        (folder / name).write_bytes(data)
    bad = bytearray((folder / 'ustar.tar').read_bytes()); bad[0] ^= 1
    (folder / 'bad-checksum.tar').write_bytes(bad)
    manifest = {'name': '@cortex/owned-fixture', 'version': '1.2.3', 'main': 'index.js',
                'scripts': {'postinstall': 'DO_NOT_EXECUTE_THIS_FIXTURE'}, 'dependencies': {'example': '^1.0.0'}}
    npm = tar_bytes(tarfile.PAX_FORMAT, [('package/package.json', json.dumps(manifest).encode(), tarfile.REGTYPE),
                                         ('package/index.js', b'throw new Error("never execute");\n', tarfile.REGTYPE)])
    (folder / 'package.tar').write_bytes(npm)
    (folder / 'package.tgz').write_bytes(gzip.compress(npm, mtime=0))
    (folder / 'plain.gz').write_bytes(gzip.compress(PAYLOAD, mtime=0))
    (folder / 'concatenated.gz').write_bytes(gzip.compress(b'first', mtime=0) + gzip.compress(b'second', mtime=0))
    raw = zlib.compressobj(wbits=-15)
    compressed = raw.compress(PAYLOAD) + raw.flush()
    header = b'\x1f\x8b\x08\x1e' + struct.pack('<I', 0) + b'\x00\xff'
    header += struct.pack('<H', 4) + b'abcd' + b'../name-must-not-escape\x00' + b'comment\x00'
    header += struct.pack('<H', zlib.crc32(header) & 0xffff)
    optional = header + compressed + struct.pack('<II', zlib.crc32(PAYLOAD), len(PAYLOAD))
    (folder / 'optional-header.gz').write_bytes(optional)
    bad = bytearray(optional); bad[-8] ^= 0x40
    (folder / 'bad-crc.gz').write_bytes(bad)
    # A header checksum corruption, separate from the payload trailer.
    bad = bytearray(optional); bad[10] ^= 1
    (folder / 'bad-header.gz').write_bytes(bad)
    (folder / 'simple.a').write_bytes(ar_bytes([('alpha.bin', PAYLOAD), ('note.txt', b'notes')]))
    (folder / 'bsd.a').write_bytes(ar_bytes([('long-name-that-does-not-fit.bin', PAYLOAD)], bsd=True))
    # GNU filename table and a reference to it.
    names = b'very-long-gnu-filename.bin/\n'
    def raw_member(name, data):
        return (name.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6) + b'100644'.ljust(8)
                + str(len(data)).encode().ljust(10) + b'`\n' + data + (b'\n' if len(data) & 1 else b''))
    (folder / 'gnu.a').write_bytes(b'!<arch>\n' + raw_member(b'//', names) + raw_member(b'/0', PAYLOAD))
    (folder / 'thin.a').write_bytes(b'!<thin>\n')
    (folder / 'unsafe.a').write_bytes(ar_bytes([('../escape', PAYLOAD)]))
    control = tar_bytes(tarfile.USTAR_FORMAT, [('control', b'Package: cortex-fixture\nVersion: 1.2.3\n', tarfile.REGTYPE)])
    contents = tar_bytes(tarfile.USTAR_FORMAT, [('usr/share/cortex/hello.txt', PAYLOAD, tarfile.REGTYPE)])
    deb = ar_bytes([('debian-binary', b'2.0\n'), ('control.tar.gz', gzip.compress(control, mtime=0)),
                    ('data.tar.gz', gzip.compress(contents, mtime=0))])
    (folder / 'sample.deb').write_bytes(deb)
    # Independent read-back by stdlib, never importing package code.
    assert gzip.decompress(optional) == PAYLOAD
    assert gzip.decompress((folder / 'concatenated.gz').read_bytes()) == b'firstsecond'
    for name in ('ustar.tar', 'v7.tar', 'pax.tar', 'gnu.tar'):
        with tarfile.open(folder / name, 'r:') as t:
            assert t.extractfile(t.getmembers()[0]).read() == PAYLOAD
    (folder / 'expected.json').write_text(json.dumps({'payload_sha256': hashlib.sha256(PAYLOAD).hexdigest(),
        'npm': manifest, 'producers': ['Python tarfile', 'gzip', 'zlib', 'struct']}, indent=2), encoding='utf-8')
    print('archive fixtures:', folder)


def build_certificates(folder, openssl):
    folder.mkdir(parents=True, exist_ok=True)
    def run(*args):
        result = subprocess.run([openssl, *map(str, args)], capture_output=True)
        if result.returncode:
            raise RuntimeError('OpenSSL failed: ' + result.stderr.decode('utf-8', errors='replace'))
        return result.stdout
    with tempfile.TemporaryDirectory(prefix='cortex-public-certificate-fixtures-') as temp:
        key = Path(temp) / 'test-key.pem'
        cert = folder / 'certificate.pem'
        run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
            '-days', '3650', '-set_serial', '42', '-subj', '/CN=Cortex Test Certificate/O=Cortex Fixture/C=FR',
            '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'subjectAltName=DNS:fixture.invalid')
        run('x509', '-in', cert, '-outform', 'DER', '-out', folder / 'certificate.der')
        run('crl2pkcs7', '-nocrl', '-certfile', cert, '-outform', 'DER', '-out', folder / 'certificates.p7b')
        payload = folder / 'content.bin'
        payload.write_bytes(b'Owned certificate fixture. This is data, not a script.\n')
        for name, options in [('signed.p7m', []), ('catalog.cat', ['-econtent_type', '1.3.6.1.4.1.311.10.1'])]:
            run('cms', '-sign', '-binary', '-nodetach', '-in', payload, '-signer', cert, '-inkey', key,
                '-outform', 'DER', '-out', folder / name, *options)
        # Reference verification occurs in this generator only. Cortex does not
        # inherit this oracle result as a verification of arbitrary signatures.
        result = run('cms', '-verify', '-binary', '-inform', 'DER', '-in', folder / 'signed.p7m', '-noverify')
        assert result == payload.read_bytes()
        public_text = run('x509', '-in', cert, '-noout', '-subject', '-issuer', '-serial', '-fingerprint', '-sha256').decode()
        (folder / 'openssl-expected.txt').write_text(public_text, encoding='utf-8')
        pem = cert.read_bytes()
        (folder / 'bundle.pem').write_bytes(pem + pem)
        cms = (folder / 'signed.p7m').read_bytes()
        (folder / 'signed.pem').write_bytes(b'-----BEGIN CMS-----\n' + base64.encodebytes(cms) + b'-----END CMS-----\n')
        (folder / 'bad-base64.pem').write_bytes(b'-----BEGIN CERTIFICATE-----\n?===\n-----END CERTIFICATE-----\n')
        (folder / 'indefinite.der').write_bytes(b'\x30\x80\x02\x01\x01\x00\x00')
    print('public certificate fixtures:', folder)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=HERE)
    p.add_argument('--openssl', default=shutil.which('openssl'))
    p.add_argument('--archives-only', action='store_true')
    args = p.parse_args()
    build_archives(args.out / 'archive-next')
    if not args.archives_only:
        if not args.openssl:
            raise SystemExit('OpenSSL required only to generate public certificate fixtures')
        build_certificates(args.out / 'certificates', args.openssl)
