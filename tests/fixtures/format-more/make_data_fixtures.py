#!/usr/bin/env python3
"""Builds the data and model fixtures of tests/fixtures/format-more/data and
the reference values the C++ tests compare against. Each file is written by
the format's own library and read back by it for the expected values:

  sample.db, wal.db-wal   sqlite3 (schema, row counts, WAL frame count from
                          PRAGMA wal_checkpoint)
  sample.bplist           plistlib (binary), incl. an NSKeyedArchiver layout
  *.pkl, model.pt         pickle; expected opcodes from pickletools and
                          imports from an Unpickler whose find_class only
                          records and returns an inert stub. The os.system
                          pickle is only serialized here, never executed.
  *.npy, arrays.npz       numpy.save / numpy.savez
  weights.safetensors     safetensors.numpy
  model.gguf              gguf-py GGUFWriter, read back with GGUFReader
  model.onnx              onnx.helper, checked with onnx.checker

Needs `pip install numpy safetensors gguf onnx` (CPython 3.13).
"""
import collections
import datetime
import io
import json
import os
import pickle
import pickletools
import plistlib
import shutil
import sqlite3
import sys
import types
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "data")
os.makedirs(OUT, exist_ok=True)
expected = {}

# ------------------------------------------------------------------ SQLite ---
db = os.path.join(OUT, "sample.db")
for p in (db, db + "-journal"):
    if os.path.exists(p):
        os.remove(p)
con = sqlite3.connect(db)
con.executescript("""
PRAGMA page_size=1024;
PRAGMA user_version=7;
PRAGMA application_id=1129271892;
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, bio TEXT, score REAL, avatar BLOB);
CREATE INDEX users_name ON users(name);
CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;
CREATE VIEW top_users AS SELECT name FROM users WHERE score > 50;
CREATE TRIGGER users_audit AFTER INSERT ON users BEGIN SELECT 1; END;
""")
for i in range(300):
    con.execute("INSERT INTO users(name, bio, score, avatar) VALUES (?,?,?,?)",
                (f"user{i}", "bio " * (i % 40), i * 0.5, bytes([i % 256]) * (i % 7)))
con.execute("INSERT INTO users(name, bio) VALUES (?,?)", ("long", "x" * 10000))  # overflow pages
for i in range(50):
    con.execute("INSERT INTO settings VALUES (?,?)", (f"key{i:03}", "v" * i))
con.commit()
schema = [list(r) for r in con.execute("SELECT type, name, tbl_name, rootpage, sql FROM sqlite_master")]
counts = {t: con.execute(f'SELECT count(*) FROM "{t}"').fetchone()[0]
          for (t,) in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
con.close()

import tempfile
wal_dir = tempfile.mkdtemp()
wal = os.path.join(wal_dir, "wal.db")  # working copy; only its -wal file is kept
wcon = sqlite3.connect(wal)
wcon.execute("PRAGMA journal_mode=WAL")
wcon.execute("PRAGMA wal_autocheckpoint=0")
wcon.execute("CREATE TABLE log(id INTEGER PRIMARY KEY, msg TEXT)")
for batch in range(3):
    wcon.executemany("INSERT INTO log(msg) VALUES (?)", [(f"event {batch}-{i}",) for i in range(20)])
    wcon.commit()
shutil.copyfile(wal + "-wal", os.path.join(OUT, "wal.db-wal"))
busy, frames, done = wcon.execute("PRAGMA wal_checkpoint(PASSIVE)").fetchone()
wal_page = wcon.execute("PRAGMA page_size").fetchone()[0]
wcon.close()
shutil.rmtree(wal_dir)
expected["sqlite"] = {"schema": schema, "counts": counts, "page_size": 1024, "user_version": 7,
                      "application_id": 1129271892, "wal_frames": frames, "wal_page_size": wal_page}

# ------------------------------------------------------------------ bplist ---
uid = plistlib.UID
archive = {"$archiver": "NSKeyedArchiver", "$version": 100000, "$top": {"root": uid(1)},
           "$objects": ["$null", {"$class": uid(2), "name": uid(3)},
                        {"$classname": "CortexFixture", "$classes": ["CortexFixture", "NSObject"]}, "owned"]}
plist = {"ascii": "hello", "unicode": "héllo ✓", "int": 42, "negative": -7, "big": 2 ** 40, "real": 1.5,
         "true": True, "false": False, "data": b"\x00\x01cortex", "date": datetime.datetime(2024, 1, 2, 3, 4, 5),
         "array": [1, "two", [3.0]], "nested": {"k": "v"}}
with open(os.path.join(OUT, "sample.bplist"), "wb") as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY, sort_keys=True)
with open(os.path.join(OUT, "keyed.bplist"), "wb") as f:
    plistlib.dump(archive, f, fmt=plistlib.FMT_BINARY)
epoch = datetime.datetime(2001, 1, 1)
expected["bplist"] = {"date_seconds": (plist["date"] - epoch).total_seconds(), "data_hex": plist["data"].hex(),
                      "big": plist["big"], "unicode": plist["unicode"], "classes": ["CortexFixture", "NSObject"]}

# ------------------------------------------------------------------ pickle ---
class Evil:
    def __reduce__(self):
        import os as _os
        return (_os.system, ("echo cortex-fixture-never-run",))

benign = {"list": [1, 2.5, None, True], "tuple": (b"bytes", "text"), "set": {1, 2},
          "big": 2 ** 100, "nested": {"again": "text"}}
pickles = {
    "benign.pkl": pickle.dumps(benign, protocol=4),
    "ordered.pkl": pickle.dumps(collections.OrderedDict(a=datetime.date(2024, 5, 6)), protocol=2),
    "evil.pkl": pickle.dumps(Evil(), protocol=5),
}

# A PyTorch-style checkpoint without torch: the same module paths and a
# persistent id per storage, inside the zip layout torch.save writes.
torch_utils = types.ModuleType("torch._utils")
torch_mod = types.ModuleType("torch")
def _rebuild_tensor_v2(*args):
    raise RuntimeError("fixture stub")
torch_utils._rebuild_tensor_v2 = _rebuild_tensor_v2
_rebuild_tensor_v2.__module__ = "torch._utils"
class FloatStorage:
    pass
FloatStorage.__module__ = "torch"
torch_mod.FloatStorage = FloatStorage
torch_mod._utils = torch_utils
sys.modules["torch"] = torch_mod
sys.modules["torch._utils"] = torch_utils
class FakeTensor:
    def __reduce__(self):
        return (_rebuild_tensor_v2, (StorageRef(), 0, (2, 2), (2, 1), False, collections.OrderedDict()))
class StorageRef:
    pass
class TorchPickler(pickle.Pickler):
    def persistent_id(self, obj):
        if isinstance(obj, StorageRef):
            return ("storage", FloatStorage, "0", "cpu", 4)
        return None
buf = io.BytesIO()
TorchPickler(buf, protocol=2).dump(collections.OrderedDict(weight=FakeTensor()))
pickles["torch-data.pkl"] = buf.getvalue()
with zipfile.ZipFile(os.path.join(OUT, "model.pt"), "w", zipfile.ZIP_STORED) as z:
    z.writestr("model/data.pkl", pickles["torch-data.pkl"])
    z.writestr("model/data/0", np.arange(4, dtype=np.float32).tobytes())
    z.writestr("model/version", "3\n")

class Inert:
    """Accepts every call, item and state the unpickler hands it; does nothing."""
    def __init__(self, *a, **k): pass
    def __call__(self, *a, **k): return Inert()
    def __setitem__(self, k, v): pass
    def __setstate__(self, state): pass
    def append(self, x): pass
    def extend(self, x): pass

class Recorder(pickle.Unpickler):
    def __init__(self, data):
        super().__init__(io.BytesIO(data))
        self.imports = []
    def find_class(self, module, name):
        self.imports.append([module, name])
        return Inert   # nothing real is imported or called
    def persistent_load(self, pid):
        return None

pickle_ref = {}
for name, data in pickles.items():
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)
    r = Recorder(data)
    r.load()
    pickle_ref[name] = {"ops": [[pos, op.name] for op, arg, pos in pickletools.genops(data)], "imports": r.imports}
expected["pickle"] = pickle_ref

# ------------------------------------------------------------------- NumPy ---
arrays = {
    "f32.npy": np.arange(12, dtype=np.float32).reshape(3, 4),
    "i64.npy": np.arange(5, dtype=np.int64),
    "fortran.npy": np.asfortranarray(np.ones((2, 3), dtype=np.float64)),
    "text.npy": np.array(["ab", "cortex"]),
    "object.npy": np.array([{"a": 1}, None], dtype=object),
    "struct.npy": np.zeros(3, dtype=[("x", "<i4"), ("y", "<f8")]),
}
npy_ref = {}
for name, a in arrays.items():
    np.save(os.path.join(OUT, name), a, allow_pickle=True)
    npy_ref[name] = {"shape": list(a.shape), "descr": a.dtype.str if a.dtype.fields is None else "structured",
                     "fortran": bool(a.flags.f_contiguous and not a.flags.c_contiguous)}
np.savez(os.path.join(OUT, "arrays.npz"), first=arrays["f32.npy"], second=arrays["i64.npy"])
expected["npy"] = npy_ref

# ------------------------------------------------------------- safetensors ---
from safetensors.numpy import save_file
tensors = {"weight": np.ones((4, 3), dtype=np.float32), "bias": np.zeros(3, dtype=np.float16),
           "ids": np.arange(5, dtype=np.int64)}
st = os.path.join(OUT, "weights.safetensors")
save_file(tensors, st, metadata={"owner": "cortex", "format": "np"})
with open(st, "rb") as f:
    n = int.from_bytes(f.read(8), "little")
    header = json.loads(f.read(n))
expected["safetensors"] = {"metadata": header["__metadata__"],
                           "tensors": {k: {"dtype": v["dtype"], "shape": v["shape"]} for k, v in header.items() if k != "__metadata__"}}

# -------------------------------------------------------------------- GGUF ---
import gguf
path = os.path.join(OUT, "model.gguf")
w = gguf.GGUFWriter(path, "llama")
w.add_name("cortex-fixture")
w.add_uint32("cortex.answer", 42)
w.add_float32("cortex.scale", 0.25)
w.add_bool("cortex.owned", True)
w.add_array("tokenizer.ggml.tokens", ["<s>", "</s>", "cortex"])
w.add_tensor("token_embd.weight", np.ones((3, 8), dtype=np.float32))
w.add_tensor("output_norm.weight", np.ones(8, dtype=np.float16))
w.write_header_to_file(); w.write_kv_data_to_file(); w.write_tensors_to_file(); w.close()
reader = gguf.GGUFReader(path)
expected["gguf"] = {"fields": sorted(reader.fields.keys()),
                    "tensors": {t.name: {"shape": [int(x) for x in t.shape], "type": t.tensor_type.name} for t in reader.tensors},
                    "answer": 42, "name": "cortex-fixture"}

# -------------------------------------------------------------------- ONNX ---
import onnx
from onnx import helper, TensorProto, numpy_helper
W = numpy_helper.from_array(np.ones((4, 3), dtype=np.float32), "W")
B = numpy_helper.from_array(np.zeros(3, dtype=np.float32), "B")
graph = helper.make_graph(
    [helper.make_node("MatMul", ["X", "W"], ["XW"], name="matmul"),
     helper.make_node("Add", ["XW", "B"], ["Z"], name="add"),
     helper.make_node("Relu", ["Z"], ["Y"], name="relu")],
    "cortex_graph",
    [helper.make_tensor_value_info("X", TensorProto.FLOAT, [1, 4])],
    [helper.make_tensor_value_info("Y", TensorProto.FLOAT, [1, 3])],
    [W, B])
model = helper.make_model(graph, producer_name="cortex-fixture", opset_imports=[helper.make_opsetid("", 17)])
model.doc_string = "owned test model"
helper.set_model_props(model, {"owner": "cortex"})
onnx.checker.check_model(model)
onnx.save(model, os.path.join(OUT, "model.onnx"))
m = onnx.load(os.path.join(OUT, "model.onnx"))
expected["onnx"] = {"ir_version": m.ir_version, "producer": m.producer_name,
                    "opset": [[o.domain, o.version] for o in m.opset_import],
                    "graph": m.graph.name, "ops": [n.op_type for n in m.graph.node],
                    "inputs": [i.name for i in m.graph.input], "outputs": [o.name for o in m.graph.output],
                    "initializers": [[t.name, list(t.dims)] for t in m.graph.initializer], "metadata": {"owner": "cortex"}}

with open(os.path.join(OUT, "expected.json"), "w") as f:
    json.dump(expected, f, indent=1, sort_keys=True, default=str)
print("fixtures written to", OUT)
