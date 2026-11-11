#!/usr/bin/env python3
"""Generate owned metadata-only packages with Python zipfile/protobuf encoding.

Extension scripts are deliberately inert test text and are never run. CRX
proofs are structural placeholders, NOT cryptographically valid signatures.
JMOD initially validates the container only, not a Java module descriptor.
"""
from pathlib import Path
import argparse
import io
import json
import struct
import zipfile

HERE = Path(__file__).resolve().parent
MANIFEST = {'manifest_version': 3, 'name': 'Cortex Extension Fixture', 'version': '1.2.3',
            'permissions': ['storage', 'tabs'], 'host_permissions': ['https://fixture.invalid/*'],
            'background': {'service_worker': 'worker.js'},
            'content_scripts': [{'matches': ['https://fixture.invalid/*'], 'js': ['content.js'], 'run_at': 'document_idle'}],
            'browser_specific_settings': {'gecko': {'id': 'fixture@cortex.invalid'}}}
SCRIPT = b'throw new Error("Cortex parser fixture: never execute");\n'


def zipped(items):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for name, content in items:
            i = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            i.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(i, content)
    return out.getvalue()


def vint(number):
    out = bytearray()
    while number > 127:
        out.append((number & 127) | 128)
        number >>= 7
    out.append(number)
    return bytes(out)


def field(number, value):
    return vint((number << 3) | 2) + vint(len(value)) + value


def main(out):
    out.mkdir(parents=True, exist_ok=True)
    content = [('manifest.json', '// Owned fixture\n' + json.dumps(MANIFEST)), ('worker.js', SCRIPT), ('content.js', SCRIPT)]
    archive = zipped(content)
    (out / 'sample.xpi').write_bytes(archive)
    (out / 'webextension.zip').write_bytes(archive)
    (out / 'invalid-extension.zip').write_bytes(zipped([('manifest.json', '{"name":"not a webextension"}')]))
    # CRX2 bounded public-key/signature fields (intentionally not valid proofs).
    key, signature = b'fixture-public-key', b'fixture-signature'
    (out / 'sample2.crx').write_bytes(b'Cr24' + struct.pack('<III', 2, len(key), len(signature)) + key + signature + archive)
    proof = field(1, key) + field(2, signature)
    signed = field(1, bytes(range(16)))
    header = field(2, proof) + field(10000, signed)
    crx3 = b'Cr24' + struct.pack('<II', 3, len(header)) + header + archive
    (out / 'sample3.crx').write_bytes(crx3)
    (out / 'oversized.crx').write_bytes(b'Cr24' + struct.pack('<II', 3, 0xffffffff) + archive)
    duplicate = header + field(10000, signed)
    (out / 'duplicate-header.crx').write_bytes(b'Cr24' + struct.pack('<II', 3, len(duplicate)) + duplicate + archive)
    # VS Code's VSIX package combines the Microsoft XML manifest and package.json.
    vsix = '''<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011">
  <Metadata><Identity Id="cortex.fixture" Version="2.3.4" Publisher="Cortex" Language="en-US"/>
    <DisplayName>Cortex Fixture</DisplayName><Description>Metadata only</Description></Metadata>
  <Dependencies><Dependency Id="Microsoft.VisualStudio.Code" Version="[1.0,)"/></Dependencies>
  <Assets><Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json"/></Assets>
</PackageManifest>'''
    vscode = {'name': 'fixture', 'publisher': 'cortex', 'version': '2.3.4', 'main': './out/extension.js',
              'engines': {'vscode': '^1.90.0'}, 'activationEvents': ['onCommand:cortex.fixture']}
    (out / 'sample.vsix').write_bytes(zipped([('extension.vsixmanifest', vsix), ('[Content_Types].xml', '<Types/>'),
                                             ('extension/package.json', json.dumps(vscode)), ('extension/out/extension.js', SCRIPT)]))
    (out / 'invalid.vsix').write_bytes(zipped([('extension.vsixmanifest', '<!DOCTYPE x><PackageManifest/>')]))
    war = '<web-app xmlns="https://jakarta.ee/xml/ns/jakartaee" version="6.0"><display-name>Fixture</display-name><servlet><servlet-name>Hello</servlet-name><servlet-class>fixture.Hello</servlet-class></servlet><servlet-mapping><servlet-name>Hello</servlet-name><url-pattern>/hello</url-pattern></servlet-mapping></web-app>'
    ear = '<application xmlns="https://jakarta.ee/xml/ns/jakartaee" version="10"><display-name>Fixture Enterprise</display-name><module><web><web-uri>sample.war</web-uri><context-root>/fixture</context-root></web></module></application>'
    war_bytes = zipped([('WEB-INF/web.xml', war), ('index.html', '<p>Fixture</p>')])
    (out / 'sample.war').write_bytes(war_bytes)
    (out / 'sample.ear').write_bytes(zipped([('META-INF/application.xml', ear), ('sample.war', war_bytes)]))
    (out / 'sample.pyz').write_bytes(zipped([('__main__.py', 'raise RuntimeError("never execute")\n')]))
    wheel_metadata = 'Metadata-Version: 2.1\nName: cortex-fixture\nVersion: 1.2.3\nRequires-Python: >=3.9\nRequires-Dist: alpha>=1\nRequires-Dist: beta; python_version>"3.10"\n\nDescription\n'
    (out / 'sample.whl').write_bytes(zipped([('cortex_fixture-1.2.3.dist-info/WHEEL', 'Wheel-Version: 1.0\nRoot-Is-Purelib: true\n'),
                                           ('cortex_fixture-1.2.3.dist-info/METADATA', wheel_metadata), ('cortex_fixture/__init__.py', '# never imported\n')]))
    # JMOD container entry can be read without executing or validating Java bytecode.
    (out / 'container.jmod').write_bytes(b'JM\x01\x00' + zipped([('classes/module-info.class', b'\xca\xfe\xba\xbe'),
                                                               ('legal/LICENSE', b'Owned container fixture')]))
    release = ('a' * 40 + ' Cortex-1.2.3-full.nupkg 12345\n' + 'b' * 40 + ' https://fixture.invalid/Cortex-1.2.4-delta.nupkg?token=not-a-secret 678 # 25%\n')
    (out / 'RELEASES').write_text(release, encoding='utf-8')
    (out / 'invalid-RELEASES.txt').write_text('a' * 40 + ' ../escape.nupkg 1\n', encoding='utf-8')
    (out / 'expected.json').write_text(json.dumps({'manifest': MANIFEST, 'vscode': vscode, 'crx_signature_valid': False,
                                                'crx_id': bytes(range(16)).hex(), 'jmod_scope': 'container only'}, indent=2), encoding='utf-8')
    # Independent zipfile checks all supported ZIP wrappers without installation.
    for name in ('sample.xpi', 'sample.vsix', 'sample.war', 'sample.ear', 'sample.pyz', 'sample.whl', 'sample2.crx', 'sample3.crx', 'container.jmod'):
        with zipfile.ZipFile(out / name) as z:
            assert z.testzip() is None
    print('package fixtures:', out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=HERE / 'package-next')
    main(parser.parse_args().out)
