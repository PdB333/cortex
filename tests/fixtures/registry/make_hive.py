#!/usr/bin/env python3
"""Writes tests/fixtures/registry/small.hiv, a minimal registry hive.

ROOT
  Software
    Cortex          Name (REG_SZ), Count (REG_DWORD), Data (REG_BINARY),
                    List (REG_MULTI_SZ), Big (REG_QWORD), Path (REG_EXPAND_SZ)
    Empty
  System

The layout follows the documented format (base block, one hbin, nk, vk, lf
and value-list cells). python-registry reads the result back as the
independent check; tests/artifacts_tests.cpp compares Cortex's reading.

    python3 tests/fixtures/registry/make_hive.py
"""

import os
import struct

FILETIME = 133_700_000_000_000_000  # 2024-09-03


class Bin:
    def __init__(self):
        self.data = bytearray()

    def cell(self, payload):
        size = len(payload) + 4
        size += (-size) % 8
        offset = 0x20 + len(self.data)
        self.data += struct.pack("<i", -size) + payload + b"\0" * (size - 4 - len(payload))
        return offset


def nk(name, parent, subkeys, subkey_list, values, value_list, flags=0x20):
    raw = name.encode("latin-1")
    return (b"nk" + struct.pack("<HQIIIIIIIIIIIIIIIHH", flags, FILETIME, 0, parent, subkeys, 0, subkey_list,
                                0xFFFFFFFF, values, value_list, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 0, 0, len(raw), 0)
            + raw)


def vk(name, data_type, data, cells):
    raw = name.encode("latin-1")
    if len(data) <= 4:
        size = len(data) | 0x80000000
        offset = struct.unpack("<I", data.ljust(4, b"\0"))[0]
    else:
        size = len(data)
        offset = cells.cell(data)
    return b"vk" + struct.pack("<HIIIHH", len(raw), size, offset, data_type, 1, 0) + raw


def utf16z(text):
    return text.encode("utf-16-le") + b"\0\0"


def main():
    cells = Bin()
    # Offsets are known once cells are written, so keys are written after
    # their values, children before parents, then patched.
    values = [
        ("Name", 1, utf16z("Cortex fixture")),
        ("Count", 4, struct.pack("<I", 42)),
        ("Data", 3, bytes([1, 2, 3])),
        ("List", 7, utf16z("alpha") + utf16z("beta") + b"\0\0"),
        ("Big", 11, struct.pack("<Q", 0x0102030405060708)),
        ("Path", 2, utf16z("%SystemRoot%\\cortex")),
    ]
    value_cells = [cells.cell(vk(name, kind, data, cells)) for name, kind, data in values]
    value_list = cells.cell(b"".join(struct.pack("<I", offset) for offset in value_cells))

    placeholder = 0xFFFFFFFF
    cortex = cells.cell(nk("Cortex", placeholder, 0, 0xFFFFFFFF, len(values), value_list))
    empty = cells.cell(nk("Empty", placeholder, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF))

    def lf(children):
        body = b"lf" + struct.pack("<H", len(children))
        for offset, name in children:
            body += struct.pack("<I", offset) + name[:4].encode("latin-1").ljust(4, b"\0")
        return cells.cell(body)

    software_list = lf([(cortex, "Cortex"), (empty, "Empty")])
    software = cells.cell(nk("Software", placeholder, 2, software_list, 0, 0xFFFFFFFF))
    system = cells.cell(nk("System", placeholder, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF))
    root_list = lf([(software, "Soft"), (system, "Syst")])
    root = cells.cell(nk("ROOT", 0xFFFFFFFF, 2, root_list, 0, 0xFFFFFFFF, flags=0x2C))

    def set_parent(child, parent):
        at = child - 0x20 + 4 + 16
        cells.data[at:at + 4] = struct.pack("<I", parent)

    for child, parent in ((cortex, software), (empty, software), (software, root), (system, root)):
        set_parent(child, parent)

    body = bytes(cells.data)
    size = 0x20 + len(body)
    size += (-size) % 0x1000
    free = size - 0x20 - len(body)
    if free:
        body += struct.pack("<i", free) + b"\0" * (free - 4)
    hbin = b"hbin" + struct.pack("<IIQQI", 0, size, 0, FILETIME, 0) + body

    base = bytearray(0x1000)
    struct.pack_into("<4sIIQIIIIII", base, 0, b"regf", 1, 1, FILETIME, 1, 5, 0, 1, root, len(hbin))
    base[48:48 + 64] = "\\??\\C:\\cortex\\small.hiv".encode("utf-16-le").ljust(64, b"\0")[:64]
    checksum = 0
    for i in range(127):
        checksum ^= struct.unpack_from("<I", base, i * 4)[0]
    struct.pack_into("<I", base, 508, checksum)

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "small.hiv")
    with open(path, "wb") as handle:
        handle.write(bytes(base) + hbin)
    print(path)


if __name__ == "__main__":
    main()
