"""Reproducible MSF7/PDB structural fixtures. No input code is executed.

Layout follows LLVM's MSF/PDB documentation and CodeView record definitions.
The synthetic files exercise GUID/age, DBI, public/private symbols, TPI/IPI,
section RVAs and C13 lines, including fragmented 512-byte streams.
"""
from pathlib import Path
import struct


def u16(value):
    return struct.pack('<H', value)


def u32(value):
    return struct.pack('<I', value)


def record(kind, payload):
    payload += b'\x00' * (-(4 + len(payload)) % 4)
    return u16(len(payload) + 2) + u16(kind) + payload


def subsection(kind, payload):
    return u32(kind) + u32(len(payload)) + payload + b'\x00' * (-len(payload) % 4)


def tpi(records):
    header = bytearray(56)
    struct.pack_into('<IIIIIHH', header, 0, 20040203, 56, 0x1000,
                     0x1000 + len(records), sum(map(len, records)), 0xffff, 0xffff)
    return bytes(header) + b''.join(records)


def streams(machine):
    info = struct.pack('<III', 20000404, 0x12345678, 1)
    info += bytes.fromhex('33221100554477668899aabbccddeeff')
    info += u32(0) + u32(0) + u32(1) + u32(0) + u32(0)  # empty named stream map
    point = record(0x1505, struct.pack('<HHIII', 2, 0, 0, 0, 0) + u16(8) + b'Point\x00')
    types = tpi([point])
    public = record(0x110e, struct.pack('<IIH', 2, 0x10, 1) + b'CortexPdbAdd\x00')
    data = record(0x110d, struct.pack('<IIH', 0x74, 0, 2) + b'cortex_sink\x00')
    # Enough records to span several 512-byte blocks. Blocks are fragmented.
    publics = public + data + b''.join(record(0x110e, struct.pack('<IIH', 0, i, 2)
                     + ('symbol_%03d' % i).encode() + b'\x00') for i in range(60))
    proc = record(0x1110, struct.pack('<IIIIIIIIHB', 0, 0, 0, 0x20, 0, 0x20,
                  0x1000, 0x10, 1, 0) + b'CortexPdbAdd\x00')
    symbols = u32(4) + proc
    strings = subsection(0xf3, b'\x00sample.cpp\x00')
    checksums = subsection(0xf4, u32(1) + bytes([0, 0]) + b'\x00\x00')
    line_data = struct.pack('<IHHI', 0x10, 1, 0, 0x20)
    line_data += struct.pack('<III', 0, 2, 28)
    line_data += struct.pack('<IIII', 0, 0x80000004, 7, 0x80000005)
    lines = strings + checksums + subsection(0xf2, line_data)
    module = bytearray(64)
    struct.pack_into('<H', module, 34, 6)
    struct.pack_into('<III', module, 36, len(symbols), 0, len(lines))
    struct.pack_into('<H', module, 48, 1)
    module += b'sample.obj\x00sample.obj\x00'
    module += b'\x00' * (-len(module) % 4)
    source_info = struct.pack('<HHHHI', 1, 1, 0, 1, 0) + b'sample.cpp\x00'
    optional = bytearray(b'\xff' * 22)
    struct.pack_into('<H', optional, 10, 7)
    dbi = bytearray(64)
    struct.pack_into('<IIIHHHHHH', dbi, 0, 0xffffffff, 19990903, 1,
                     0xffff, 0, 0xffff, 0, 5, 0)
    struct.pack_into('<IIIII', dbi, 24, len(module), 0, 0, len(source_info), 0)
    struct.pack_into('<I', dbi, 48, len(optional))
    struct.pack_into('<H', dbi, 58, machine)
    dbi += module + source_info + optional
    sections = bytearray(80)
    for i, name, rva in [(0, b'.text', 0x1000), (1, b'.data', 0x2000)]:
        sections[i*40:i*40+len(name)] = name
        struct.pack_into('<III', sections, i*40+8, 0x100, rva, 0x100)
    return [b'', info, types, bytes(dbi), tpi([]), publics, symbols + lines, bytes(sections)]


def make(block_size, machine):
    content = streams(machine)
    pages = []
    next_page = 5
    for stream in content:
        count = (len(stream) + block_size - 1) // block_size
        # Reverse physical block order to exercise non-contiguous stream reads.
        pages.append(list(reversed(range(next_page, next_page + count))))
        next_page += count
    directory = u32(len(content)) + b''.join(u32(len(s)) for s in content)
    directory += b''.join(u32(page) for group in pages for page in group)
    assert len(directory) <= block_size
    out = bytearray(next_page * block_size)
    magic = b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00'
    assert len(magic) == 32
    out[:32] = magic
    struct.pack_into('<IIIIII', out, 32, block_size, 1, next_page, len(directory), 0, 3)
    out[3*block_size:3*block_size+4] = u32(4)
    out[4*block_size:4*block_size+len(directory)] = directory
    for stream, blocks in zip(content, pages):
        for i, page in enumerate(blocks):
            chunk = stream[i*block_size:(i+1)*block_size]
            out[page*block_size:page*block_size+len(chunk)] = chunk
    return bytes(out)


if __name__ == '__main__':
    root = Path(__file__).resolve().parent
    for block_size, machine in [(512, 0x14c), (4096, 0x8664)]:
        path = root / ('synthetic-%d.pdb' % block_size)
        path.write_bytes(make(block_size, machine))
        print(path.name, path.stat().st_size)
