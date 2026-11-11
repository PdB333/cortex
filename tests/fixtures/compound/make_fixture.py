"""Reproducible, inert MS-CFB v3/v4 fixtures with FAT and MiniFAT streams."""
from pathlib import Path
import struct


def make(version):
    ss=512 if version==3 else 4096
    ndir=(5*128+ss-1)//ss
    mini_fat=1+ndir; mini_root=2+ndir; large=3+ndir
    large_count=4096//ss; sectors=large+large_count
    out=bytearray((sectors+1)*ss)
    def put(at,fmt,*values):struct.pack_into('<'+fmt,out,at,*values)
    out[:8]=bytes.fromhex('d0cf11e0a1b11ae1')
    put(24,'HHHHH',0x3e,version,0xfffe,9 if version==3 else 12,6)
    put(40,'IIIIIIIII',0 if version==3 else ndir,1,1,0,4096,mini_fat,1,0xfffffffe,0)
    put(76,'I',0)
    for i in range(1,109):put(76+4*i,'I',0xffffffff)
    fat=[0xffffffff]*(ss//4);fat[0]=0xfffffffd
    for i in range(1,1+ndir):fat[i]=i+1 if i<ndir else 0xfffffffe
    fat[mini_fat]=fat[mini_root]=0xfffffffe
    for i in range(large,sectors):fat[i]=i+1 if i+1<sectors else 0xfffffffe
    for i,value in enumerate(fat):put(ss+i*4,'I',value)
    def entry(index,name,kind,start,size,left=0xffffffff,right=0xffffffff,child=0xffffffff):
        at=ss*2+index*128;name=(name+'\0').encode('utf-16-le')
        out[at:at+len(name)]=name;put(at+64,'HBB',len(name),kind,1)
        put(at+68,'III',left,right,child);put(at+116,'IQ',start,size)
    entry(0,'Root Entry',5,mini_root,128,child=2)
    entry(1,'tiny.txt',2,0,11)
    entry(2,'large.bin',2,large,4096,left=3,right=1)
    entry(3,'folder',1,0,0,child=4)
    entry(4,'nested.txt',2,1,12)
    for i in range(ss//4):put((mini_fat+1)*ss+4*i,'I',0xfffffffe if i<2 else 0xffffffff)
    out[(mini_root+1)*ss:(mini_root+1)*ss+11]=b'Cortex tiny'
    out[(mini_root+1)*ss+64:(mini_root+1)*ss+76]=b'Cortex child'
    content=bytes((i*17+3)%256 for i in range(4096))
    out[(large+1)*ss:(large+1)*ss+4096]=content
    return bytes(out)


if __name__=='__main__':
    here=Path(__file__).resolve().parent
    for version in (3,4):
        path=here/('structured-v%d.cfb'%version)
        path.write_bytes(make(version));print(path.name,path.stat().st_size)
