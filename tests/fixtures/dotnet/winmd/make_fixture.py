"""Generate inert, minimal ECMA-335 PE/CLR fixtures for WinMD detection.

No file is executed or loaded. This is a structural WinMD fixture with one
WinRT TypeDef/interface and one abstract method. It deliberately omits WinRT's
GuidAttribute/VersionAttribute: it validates parsing, not full WinRT metadata
conformance or language projection. No external dependency is required.
"""
from pathlib import Path
import struct


def make(winrt: bool = True) -> bytes:
    strings = bytearray(b"\x00")
    offsets = {}

    def add_string(value):
        offset = len(strings)
        strings.extend(value.encode("utf-8") + b"\x00")
        offsets[value] = offset
        return offset

    for value in ("<Module>", "Cortex.WinRT.winmd", "IDemo", "Cortex.WinRT", "Ping"):
        add_string(value)

    # One instance method: void Ping() (the method has RVA=0, no IL).
    blobs = b"\x00\x03\x20\x00\x01"
    guids = bytes(range(1, 17))

    # Tables: Module(0), TypeDef(2), MethodDef(6), Assembly(32).
    valid = (1 << 0) | (1 << 2) | (1 << 6) | (1 << 32)
    tables = bytearray(struct.pack("<IBBBBQQ", 0, 2, 0, 0, 1, valid, 0))
    tables += struct.pack("<IIII", 1, 2, 1, 1)
    tables += struct.pack("<HHHHH", 0, offsets["Cortex.WinRT.winmd"], 1, 0, 0)
    tables += struct.pack("<IHHHHH", 0, offsets["<Module>"], 0, 0, 1, 1)
    flags = 0x40A1 if winrt else 0x00A1
    tables += struct.pack("<IHHHHH", flags, offsets["IDemo"], offsets["Cortex.WinRT"], 0, 1, 1)
    tables += struct.pack("<IHHHHH", 0, 0x0003, 0x05C6, offsets["Ping"], 1, 1)
    # Assembly flags 0x0200: WindowsRuntime (in the structural WinMD variant).
    tables += struct.pack("<IHHHHIHHH", 0, 1, 0, 0, 0, 0x200 if winrt else 0,
                          0, offsets["Cortex.WinRT"], 0)

    version = b"Windows Runtime 1.2\x00" if winrt else b"v4.0.30319\x00"
    version += b"\x00" * (-len(version) % 4)
    meta = bytearray(struct.pack("<IHHII", 0x424A5342, 1, 1, 0, len(version)))
    meta += version
    meta += struct.pack("<HH", 0, 4)
    streams = [("#~", tables), ("#Strings", strings), ("#GUID", guids), ("#Blob", blobs)]
    header_size = sum(8 + ((len(name) + 1 + 3) & ~3) for name, _ in streams)
    offset = len(meta) + header_size
    for name, content in streams:
        meta += struct.pack("<II", offset, len(content))
        encoded = name.encode("ascii") + b"\x00"
        meta += encoded + b"\x00" * (-len(encoded) % 4)
        offset += (len(content) + 3) & ~3
    for _, content in streams:
        meta += content
        meta += b"\x00" * (-len(content) % 4)
    if len(meta) > 0x600:
        raise RuntimeError("metadata out of section bounds")

    # PE32 DLL: one read-only .text section containing the CLR header at
    # RVA 0x1100 and the metadata root at RVA 0x1200.
    pe = bytearray(0x800)
    pe[0:2] = b"MZ"
    struct.pack_into("<I", pe, 0x3C, 0x80)
    pe[0x80:0x84] = b"PE\x00\x00"
    coff = 0x84
    struct.pack_into("<HHIIIHH", pe, coff, 0x14C, 1, 0, 0, 0, 0xE0, 0x2102)
    opt = coff + 20
    struct.pack_into("<H", pe, opt, 0x10B)
    struct.pack_into("<I", pe, opt + 16, 0)
    struct.pack_into("<I", pe, opt + 28, 0x10000000)
    struct.pack_into("<I", pe, opt + 32, 0x1000)
    struct.pack_into("<I", pe, opt + 36, 0x200)
    struct.pack_into("<I", pe, opt + 56, 0x2000)
    struct.pack_into("<I", pe, opt + 60, 0x200)
    struct.pack_into("<H", pe, opt + 68, 3)
    struct.pack_into("<H", pe, opt + 70, 0x0100)
    struct.pack_into("<I", pe, opt + 92, 16)
    struct.pack_into("<II", pe, opt + 96 + 14 * 8, 0x1100, 72)

    section = opt + 0xE0
    pe[section:section + 5] = b".text"
    struct.pack_into("<IIII", pe, section + 8, 0x600, 0x1000, 0x600, 0x200)
    struct.pack_into("<I", pe, section + 36, 0x40000040)

    clr = 0x300
    struct.pack_into("<IHHII", pe, clr, 72, 2, 5, 0x1200, len(meta))
    struct.pack_into("<I", pe, clr + 16, 1)  # ILONLY
    pe[0x400:0x400 + len(meta)] = meta
    return bytes(pe)


if __name__ == "__main__":
    here = Path(__file__).resolve().parent
    for name, winrt in [("Cortex.WinRT.winmd", True), ("not-winrt.winmd", False)]:
        path = here / name
        path.write_bytes(make(winrt))
        print(path.name, len(path.read_bytes()))
