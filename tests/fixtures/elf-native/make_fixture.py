"""Construct deterministic ELF32/64 LE/BE byte fixtures, never execute them.

Includes symbol and relocation tables, GNU build ID, DT_NEEDED, NOW/PIE
flags and GNU_STACK/RELRO. One variant uses extended ELF table counts.
The existing tests/fixtures/elf/actor.elf is the independent GCC fixture.
"""
from pathlib import Path
import struct


def make(bits, little, extended=False):
    e = '<' if little else '>'
    wide = bits == 64
    base = 0x400000
    ehsize, phsize, shsize, symsize = (64, 56, 64, 24) if wide else (52, 32, 40, 16)
    out = bytearray(0x800 + 8 * shsize)
    def put(at, fmt, *values):
        struct.pack_into(e + fmt, out, at, *values)
    ident = b'\x7fELF' + bytes([2 if wide else 1, 1 if little else 2, 1, 0, 0]) + bytes(7)
    out[:16] = ident
    if wide:
        put(16, 'HHIQQQIHHHHHH', 3, 62, 1, base + 0x500, ehsize, 0x800, 0,
            ehsize, phsize, 0xffff if extended else 4, shsize,
            0 if extended else 8, 0xffff if extended else 1)
    else:
        put(16, 'HHIIIIIHHHHHH', 3, 3, 1, base + 0x500, ehsize, 0x800, 0,
            ehsize, phsize, 4, shsize, 8, 1)
    names = bytearray(b'\0'); offsets = {}
    for s in ['.shstrtab', '.strtab', '.text', '.symtab', '.rela.text' if wide else '.rel.text', '.note.gnu.build-id', '.dynamic']:
        offsets[s] = len(names); names += s.encode() + b'\0'
    out[0x200:0x200+len(names)] = names
    strings = b'\0target\0libfixture.so\0__stack_chk_fail\0'
    out[0x300:0x300+len(strings)] = strings
    for i, name, val, size, info, shndx in [(0,0,0,0,0,0), (1,1,base+0x500,4,0x12,3), (2,22,0,0,0x12,0)]:
        if wide: put(0x380+i*symsize, 'IBBHQQ', name, info, 0, shndx, val, size)
        else: put(0x380+i*symsize, 'IIIBBH', name, val, size, info, 0, shndx)
    # A signed addend in the ELF64 variant and implicit addend in ELF32.
    if wide: put(0x420, 'QQq', base+0x500, (1 << 32) | 1, -4)
    else: put(0x420, 'II', base+0x500, (1 << 8) | 1)
    note = struct.pack(e+'III', 4, 8, 3) + b'GNU\0' + bytes.fromhex('0123456789abcdef')
    out[0x480:0x480+len(note)] = note
    out[0x500:0x504] = b'\x90\x90\x90\xc3'
    dynamic = [(5,base+0x300),(10,len(strings)),(1,8),(30,8),(0x6ffffffb,0x08000001),(0,0)]
    ds = 16 if wide else 8
    for i,(tag,value) in enumerate(dynamic): put(0x600+i*ds, 'QQ' if wide else 'II', tag,value)
    programs = [(1,7,0,base,len(out),len(out),0x1000),
                (2,6,0x600,base+0x600,len(dynamic)*ds,len(dynamic)*ds,8 if wide else 4),
                (0x6474e551,6,0,0,0,0,16),
                (0x6474e552,4,0x600,base+0x600,len(dynamic)*ds,len(dynamic)*ds,1)]
    for i,(typ,flags,off,addr,filesz,memsz,align) in enumerate(programs):
        if wide: put(ehsize+i*phsize,'IIQQQQQQ',typ,flags,off,addr,addr,filesz,memsz,align)
        else: put(ehsize+i*phsize,'IIIIIIII',typ,off,addr,addr,filesz,memsz,flags,align)
    entries = [(0,0,0,0,0,8 if extended else 0,1 if extended else 0,4 if extended else 0,0,0),
        (offsets['.shstrtab'],3,0,0,0x200,len(names),0,0,1,0),
        (offsets['.strtab'],3,2,base+0x300,0x300,len(strings),0,0,1,0),
        (offsets['.text'],1,6,base+0x500,0x500,4,0,0,1,0),
        (offsets['.symtab'],2,0,0,0x380,3*symsize,2,1,8 if wide else 4,symsize),
        (offsets['.rela.text' if wide else '.rel.text'],4 if wide else 9,0,0,0x420,24 if wide else 8,4,3,8 if wide else 4,24 if wide else 8),
        (offsets['.note.gnu.build-id'],7,2,base+0x480,0x480,len(note),0,0,4,0),
        (offsets['.dynamic'],6,3,base+0x600,0x600,len(dynamic)*ds,2,0,8 if wide else 4,ds)]
    for i,entry in enumerate(entries): put(0x800+i*shsize,'IIQQQQIIQQ' if wide else 'IIIIIIIIII',*entry)
    return bytes(out)


if __name__ == '__main__':
    here = Path(__file__).resolve().parent
    for bits in (32,64):
        for little in (True,False):
            p=here/('elf%d-%s.elf' % (bits,'le' if little else 'be'))
            p.write_bytes(make(bits,little)); print(p.name,p.stat().st_size)
    (here/'elf64-extended.elf').write_bytes(make(64,True,True))
