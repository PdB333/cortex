"""Generate CAB fixtures without running any archived content.

MSZIP blocks are produced by Python zlib. Each block after the first uses
32 KiB preset history; tests must not treat them as independent ZIP entries.
A real Microsoft makecab fixture is produced when makecab.exe is available.
"""
from pathlib import Path
import struct, zlib, random, shutil, subprocess


def checksum(data, start=0):
    whole = len(data)//4*4
    for at in range(0,whole,4): start ^= int.from_bytes(data[at:at+4],'little')
    start ^= int.from_bytes(data[whole:],'big')
    return start


def payload():
    r=random.Random(9321)
    part=bytes(r.randrange(256) for _ in range(32768))
    return part+part[-16384:]*2+b'Cortex cabinet fixture\n'*100


def make(compression, names=None):
    plain=payload()
    entries=[('a.bin',32768,0),('nested/b.bin',len(plain)-32768,32768)] if names is None else names
    files=b''
    for name,size,offset in entries:
        files += struct.pack('<IIHHHH',size,offset,0,0x5a21,0,0x80)+name.encode('utf-8')+b'\0'
    blocks=b''
    count=0
    for at in range(0,len(plain),32768):
        chunk=plain[at:at+32768]
        if compression:
            history=plain[max(0,at-32768):at]
            compressor=zlib.compressobj(9,zlib.DEFLATED,-15,zdict=history) if history else zlib.compressobj(9,zlib.DEFLATED,-15)
            data=b'CK'+compressor.compress(chunk)+compressor.flush()
        else: data=chunk
        lengths=struct.pack('<HH',len(data),len(chunk))
        blocks+=struct.pack('<I',checksum(data,checksum(lengths)))+lengths+data
        count+=1
    header=struct.pack('<4sIIIIIBBHHHHH',b'MSCF',0,44+len(files)+len(blocks),0,44,0,3,1,1,len(entries),0,13,0)
    return header+struct.pack('<IHH',44+len(files),count,compression)+files+blocks


if __name__=='__main__':
    here=Path(__file__).resolve().parent
    (here/'stored.cab').write_bytes(make(0))
    (here/'mszip-history.cab').write_bytes(make(1))
    (here/'unsafe.cab').write_bytes(make(0,[('../escape.bin',4,0),('ok.bin',8,4)]))
    (here/'expected.bin').write_bytes(payload())
    makecab=shutil.which('makecab.exe')
    if makecab:
        subprocess.run([makecab,'/D','CompressionType=MSZIP',str(here/'expected.bin'),str(here/'microsoft-mszip.cab')],check=True)
    for p in sorted(here.glob('*.cab')): print(p.name,p.stat().st_size)
