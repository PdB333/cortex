#!/usr/bin/env python3
"""Builds the game archive fixtures of tests/fixtures/format-more/games and
the reference values, read back by each format's own library:

  pak01_dir.vpk   vpk (Valve VPK v1, data stored in the directory file)
  quake.pak       vgio.quake.pak (id Software PACK)
  doom.wad        written to the WAD layout, read back with omgifol
  game.rpa        Ren'Py RPA-3.0 (zlib pickle index, XOR key in the header),
                  read back with unrpa

Needs `pip install vpk vgio omgifol unrpa`.
"""
import json
import os
import pickle
import struct
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "games")
os.makedirs(OUT, exist_ok=True)
expected = {}
FILES = {"scripts/config.txt": b"owned = true\n", "maps/start.bsp": bytes(range(64)) * 4, "readme.md": b"# cortex fixture\n"}

# ------------------------------------------------------------------ VPK ---
import vpk
with tempfile.TemporaryDirectory() as tmp:
    for name, data in FILES.items():
        os.makedirs(os.path.join(tmp, os.path.dirname(name)), exist_ok=True)
        with open(os.path.join(tmp, name), "wb") as f:
            f.write(data)
    path = os.path.join(OUT, "pak01_dir.vpk")
    vpk.new(tmp).save(path)
pak = vpk.open(path)
expected["vpk"] = {p: {"size": len(pak.get_file(p).read()), "crc": pak.get_file(p).crc32} for p in pak}

# ---------------------------------------------------------------- id PAK ---
from vgio.quake import pak as qpak
path = os.path.join(OUT, "quake.pak")
with qpak.PakFile(path, "w") as p:
    for name, data in FILES.items():
        p.writestr(name, data)
with qpak.PakFile(path) as p:
    expected["pak"] = {i.filename: i.file_size for i in p.infolist()}

# ------------------------------------------------------------------- WAD ---
lumps = [("MAP01", b""), ("THINGS", b"\x01\x00" * 10), ("CORTEX", b"owned lump"), ("ENDOOM", b"\x20\x07" * 8)]
directory_offset = 12 + sum(len(d) for _, d in lumps)
body, entries, pos = b"", b"", 12
for name, data in lumps:
    entries += struct.pack("<ii8s", pos if data else 0, len(data), name.encode())
    body += data
    pos += len(data)
path = os.path.join(OUT, "doom.wad")
with open(path, "wb") as f:
    f.write(b"PWAD" + struct.pack("<ii", len(lumps), directory_offset) + body + entries)
import omg
w = omg.WadIO(path)
expected["wad"] = {"type": "PWAD", "lumps": [[e.name, e.size] for e in w.entries]}

# ------------------------------------------------------------------- RPA ---
key = 0x42424242
path = os.path.join(OUT, "game.rpa")
header_len = 34
data, index = b"", {}
for name, content in FILES.items():
    index[name] = [(header_len + len(data) ^ key, len(content) ^ key, b"")]
    data += content
idx = zlib.compress(pickle.dumps(index, protocol=2))
with open(path, "wb") as f:
    head = b"RPA-3.0 %016x %08x\n" % (header_len + len(data), key)
    assert len(head) == header_len
    f.write(head + data + idx)
from unrpa import UnRPA
u = UnRPA(path)
with open(path, "rb") as archive:
    read_index = u.get_index(archive) if u.get_index.__code__.co_argcount > 1 else u.get_index()
files = {}
for name, entries in read_index.items():
    name = name.decode() if isinstance(name, bytes) else name
    files[name] = {"offset": entries[0][0], "length": entries[0][1]}
expected["rpa"] = {"files": files, "key": key}

with open(os.path.join(OUT, "expected.json"), "w") as f:
    json.dump(expected, f, indent=1, sort_keys=True)
print("fixtures written to", OUT)
