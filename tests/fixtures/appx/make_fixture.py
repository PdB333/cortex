"""Build inert APPX/MSIX ZIP fixtures with ElementTree/hashlib/zipfile oracles.

The packages are structural fixtures, not installable/signed applications.
The source XML is namespace-qualified. No package is installed or executed.
"""
from pathlib import Path
import zipfile, io, hashlib, base64, xml.etree.ElementTree as ET

NS='http://schemas.microsoft.com/appx/manifest/foundation/windows10'
BM='http://schemas.microsoft.com/appx/2010/blockmap'
BN='http://schemas.microsoft.com/appx/2013/bundle'


def zip_bytes(files):
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w') as z:
        for name,data in files.items():
            info=zipfile.ZipInfo(name,date_time=(2020,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
            z.writestr(info,data)
    return out.getvalue()


def make(tamper=False,dtd=False,utf16=False):
    ET.register_namespace('',NS)
    root=ET.Element('{'+NS+'}Package')
    ET.SubElement(root,'{'+NS+'}Identity',Name='Cortex.Fixture',Publisher='CN=Cortex & Test',Version='1.2.3.4',ProcessorArchitecture='x64')
    apps=ET.SubElement(root,'{'+NS+'}Applications')
    ET.SubElement(apps,'{'+NS+'}Application',Id='Main',Executable='never-run.exe',EntryPoint='Windows.FullTrustApplication')
    caps=ET.SubElement(root,'{'+NS+'}Capabilities');ET.SubElement(caps,'{'+NS+'}Capability',Name='internetClient')
    deps=ET.SubElement(root,'{'+NS+'}Dependencies');ET.SubElement(deps,'{'+NS+'}TargetDeviceFamily',Name='Windows.Desktop',MinVersion='10.0.0.0',MaxVersionTested='10.0.0.0')
    manifest=ET.tostring(root,encoding='utf-16' if utf16 else 'utf-8',xml_declaration=True)
    if dtd:manifest=b'<!DOCTYPE Package [<!ENTITY x SYSTEM "file:///do-not-read">]>'+manifest
    data=bytes((i*17+9)%256 for i in range(70000))
    files={'AppxManifest.xml':manifest,'data.bin':data,'[Content_Types].xml':b'<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/>'}
    ET.register_namespace('',BM)
    blocks=ET.Element('{'+BM+'}BlockMap',HashMethod='http://www.w3.org/2001/04/xmlenc#sha256')
    for name,payload in files.items():
        item=ET.SubElement(blocks,'{'+BM+'}File',Name=name,Size=str(len(payload)),LfhSize='0')
        for at in range(0,len(payload),65536):
            digest=base64.b64encode(hashlib.sha256(payload[at:at+65536]).digest()).decode()
            ET.SubElement(item,'{'+BM+'}Block',Hash=digest)
    files['AppxBlockMap.xml']=ET.tostring(blocks,encoding='utf-8',xml_declaration=True)
    if tamper:files['data.bin']=b'X'+data[1:]
    return zip_bytes(files)


if __name__=='__main__':
    here=Path(__file__).resolve().parent
    for name,kw in [('sample.msix',{}),('utf16.appx',{'utf16':True}),('tampered.msix',{'tamper':True}),('dtd.msix',{'dtd':True})]:
        (here/name).write_bytes(make(**kw))
    ET.register_namespace('',BN)
    root=ET.Element('{'+BN+'}Bundle',SchemaVersion='1.0')
    ET.SubElement(root,'{'+BN+'}Identity',Name='Cortex.Bundle',Publisher='CN=Cortex',Version='1.0.0.0')
    packages=ET.SubElement(root,'{'+BN+'}Packages')
    ET.SubElement(packages,'{'+BN+'}Package',Type='application',Architecture='x64',Version='1.2.3.4',FileName='sample.msix')
    (here/'sample.msixbundle').write_bytes(zip_bytes({'AppxMetadata/AppxBundleManifest.xml':ET.tostring(root,encoding='utf-8'), 'sample.msix':make()}))
    for p in here.iterdir():
        if p.suffix in ('.msix','.appx','.msixbundle'):print(p.name,p.stat().st_size)
