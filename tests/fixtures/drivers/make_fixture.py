"""Generate inert PE32/PE32+ native-subsystem .sys fixtures.

The files contain minimal PE headers, an import descriptor naming
ntoskrnl.exe!IoCreateDevice, and a one-byte .text placeholder.  They are
parser fixtures only: never load them as Windows kernel drivers.
No third-party package is required.
"""
from pathlib import Path
from struct import pack_into


def create(bitness: int) -> bytes:
    if bitness not in (32, 64):
        raise ValueError(bitness)
    data = bytearray(0x600)

    def u16(at: int, value: int) -> None:
        pack_into("<H", data, at, value)

    def u32(at: int, value: int) -> None:
        pack_into("<I", data, at, value)

    def u64(at: int, value: int) -> None:
        pack_into("<Q", data, at, value)

    # DOS stub, NT signature, COFF header and optional header.
    data[:2] = b"MZ"
    u32(0x3C, 0x80)
    data[0x80:0x84] = b"PE\x00\x00"
    coff = 0x84
    u16(coff, 0x8664 if bitness == 64 else 0x14C)
    u16(coff + 2, 2)
    u16(coff + 16, 0xF0 if bitness == 64 else 0xE0)
    u16(coff + 18, 0x2022)  # executable, DLL, large-address-aware
    opt = coff + 20
    u16(opt, 0x20B if bitness == 64 else 0x10B)
    u32(opt + 16, 0x1000)  # entry RVA is metadata, not an actual DriverEntry
    if bitness == 64:
        u64(opt + 24, 0x140000000)
    else:
        u32(opt + 28, 0x10000000)
    u32(opt + 56, 0x3000)
    u32(opt + 60, 0x200)
    u16(opt + 68, 1)       # IMAGE_SUBSYSTEM_NATIVE
    u16(opt + 70, 0x2140)  # WDM_DRIVER, NX_COMPAT, DYNAMIC_BASE
    directory_count = opt + (108 if bitness == 64 else 92)
    directory_table = opt + (112 if bitness == 64 else 96)
    u32(directory_count, 16)
    u32(directory_table + 8, 0x2000)  # import directory RVA
    u32(directory_table + 12, 40)

    sections = opt + (0xF0 if bitness == 64 else 0xE0)
    for index, (name, rva, off, flags) in enumerate((
        (b".text", 0x1000, 0x200, 0x60000020),
        (b".idata", 0x2000, 0x400, 0xC0000040),
    )):
        at = sections + 40 * index
        data[at:at + len(name)] = name
        u32(at + 8, 0x200)
        u32(at + 12, rva)
        u32(at + 16, 0x200)
        u32(at + 20, off)
        u32(at + 36, flags)

    data[0x200] = 0xC3  # inert placeholder (never executed by the test)
    # Import directory, lookup/IAT, library name, hint + function name.
    u32(0x400, 0x2040)       # OriginalFirstThunk
    u32(0x400 + 12, 0x2060)  # Name
    u32(0x400 + 16, 0x2040)  # FirstThunk
    if bitness == 64:
        u64(0x440, 0x2080)
    else:
        u32(0x440, 0x2080)
    data[0x460:0x460 + len(b"ntoskrnl.exe\x00")] = b"ntoskrnl.exe\x00"
    data[0x480 + 2:0x480 + 2 + len(b"IoCreateDevice\x00")] = b"IoCreateDevice\x00"
    return bytes(data)


if __name__ == "__main__":
    folder = Path(__file__).resolve().parent
    for bitness in (32, 64):
        target = folder / ("fixture-x64.sys" if bitness == 64 else "fixture-x86.sys")
        target.write_bytes(create(bitness))
        print(target.name, target.stat().st_size)
