#!/usr/bin/env python3
"""Builds the bytecode fixtures of tests/fixtures/format-more and the
reference values the C++ tests compare against. Each fixture comes from the
real toolchain, and each expected value from an independent reader:

  sample313.pyc / sample313-hash.pyc  py_compile (CPython 3.13); expected
                                       names and instructions from dis
  Sample.class                        javac; expected methods and opcodes
                                       from javap -c -p
  lua51..lua54.luac, luajit2*.luac    string.dump in real Lua 5.1-5.4 and
                                       LuaJIT 2.0/2.1 (lupa wheels)
  module.wasm                         clang --target=wasm32 + wasm-ld;
                                       expected sections from llvm-readobj

Needs CPython 3.13, a JDK, clang with wasm-ld, llvm-readobj and
`pip install lupa`. The committed fixtures do not need regenerating.
"""
import dis
import importlib
import json
import marshal
import os
import py_compile
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "bytecode")
os.makedirs(OUT, exist_ok=True)
expected = {}

# ---------------------------------------------------------------- Python ----
PY_SOURCE = '''"""Cortex owned fixture."""
GREETING = "hello from cortex"

def add(a, b, *, scale=2):
    total = (a + b) * scale
    return total

class Counter:
    def bump(self, step=1):
        self.value = getattr(self, "value", 0) + step
        return self.value

def big(n):
    values = [i * 70000 for i in range(n)]
    return sum(values) + 123456789
'''
assert sys.version_info[:2] == (3, 13), "generate the .pyc fixtures with CPython 3.13"
with tempfile.TemporaryDirectory() as tmp:
    src = os.path.join(tmp, "sample.py")
    with open(src, "w") as f:
        f.write(PY_SOURCE)
    py_compile.compile(src, cfile=os.path.join(OUT, "sample313.pyc"), dfile="sample.py", doraise=True)
    py_compile.compile(src, cfile=os.path.join(OUT, "sample313-hash.pyc"), dfile="sample.py", doraise=True,
                       invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)

def code_ref(code):
    return {
        "name": code.co_name,
        "qualname": code.co_qualname,
        "first_line": code.co_firstlineno,
        "argcount": code.co_argcount,
        "kwonly_argcount": code.co_kwonlyargcount,
        "names": list(code.co_names),
        "instructions": [[i.offset, i.opname, i.arg] for i in dis.get_instructions(code)],
        "functions": [code_ref(c) for c in code.co_consts if hasattr(c, "co_code")],
    }

with open(os.path.join(OUT, "sample313.pyc"), "rb") as f:
    data = f.read()
expected["pyc"] = {"magic": int.from_bytes(data[:2], "little"), "module": code_ref(marshal.loads(data[16:]))}

# ------------------------------------------------------------------ Java ----
JAVA_SOURCE = '''package cortex.fixture;
public class Sample implements Runnable {
    public static final int LIMIT = 42;
    private String name = "owned";
    public void run() { System.out.println(name + LIMIT); }
    static int classify(int x) {
        switch (x) { case 1: return 10; case 2: return 20; case 3: return 30; default: return -1; }
    }
    static int sparse(int x) {
        switch (x) { case 10: return 1; case 1000: return 2; default: return 0; }
    }
    long total(long[] values) { long sum = 0; for (long v : values) sum += v; return sum; }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    pkg = os.path.join(tmp, "cortex", "fixture")
    os.makedirs(pkg)
    with open(os.path.join(pkg, "Sample.java"), "w") as f:
        f.write(JAVA_SOURCE)
    subprocess.run(["javac", "--release", "17", "-g", "-d", tmp, os.path.join(pkg, "Sample.java")], check=True)
    with open(os.path.join(tmp, "cortex", "fixture", "Sample.class"), "rb") as f:
        class_bytes = f.read()
    with open(os.path.join(OUT, "Sample.class"), "wb") as f:
        f.write(class_bytes)
    javap = subprocess.run(["javap", "-c", "-p", "-cp", tmp, "cortex.fixture.Sample"],
                           check=True, capture_output=True, text=True).stdout
methods, current = {}, None
for line in javap.splitlines():
    m = re.match(r"^  \S.*?([\w<>$]+)\((.*)\);$", line)
    if m:
        current = m.group(1)
        if current == "Sample":
            current = "<init>"
        methods[current] = []
        continue
    m = re.match(r"^\s+(\d+): ([a-z]\w*)", line)  # skip switch tables ("  1: 28")
    if m and current is not None:
        methods[current].append([int(m.group(1)), m.group(2)])
expected["java"] = {"class": "cortex/fixture/Sample", "super": "java/lang/Object",
                    "interfaces": ["java/lang/Runnable"], "major": class_bytes[7], "methods": methods}

# ------------------------------------------------------------------- Lua ----
LUA_SOURCE = '''local greeting = "cortex lua fixture"
local function scale(value, factor)
  local result = value * factor
  return result + 0.5
end
local counter = 0
function bump(step)
  counter = counter + step
  return greeting, scale(counter, 3)
end
return bump(1)
'''
lua = {}
for module, name in [("lua51", "lua51"), ("lua52", "lua52"), ("lua53", "lua53"), ("lua54", "lua54"),
                     ("luajit20", "luajit20"), ("luajit21", "luajit21")]:
    runtime = importlib.import_module("lupa." + module).LuaRuntime(encoding=None)  # bytes, not UTF-8 text
    loader = "loadstring" if module in ("lua51", "luajit20", "luajit21") else "load"
    # string.dump has a strip argument from Lua 5.3; LuaJIT accepts it too.
    for strip in ((False,) if module in ("lua51", "lua52") else (False, True)):
        code = f'string.dump(({loader}(...)), {"true" if strip else "false"})'.encode()
        raw = bytes(runtime.eval(code, LUA_SOURCE.encode()))
        suffix = "-stripped" if strip else ""
        with open(os.path.join(OUT, f"{name}{suffix}.luac"), "wb") as f:
            f.write(raw)
    jit = runtime.eval(b"jit and jit.version or false")
    lua[name] = {"version": runtime.eval(b"_VERSION").decode(), "jit": jit.decode() if jit else False}
expected["lua"] = lua

# ------------------------------------------------------------------ WASM ----
C_SOURCE = '''__attribute__((import_module("env"), import_name("host_log"))) int host_log(int);
int counter;
__attribute__((export_name("add"))) int add(int a, int b) { return a + b; }
__attribute__((export_name("bump"))) int bump(int step) { counter += step; return host_log(counter); }
'''
with tempfile.TemporaryDirectory() as tmp:
    c = os.path.join(tmp, "module.c")
    with open(c, "w") as f:
        f.write(C_SOURCE)
    wasm = os.path.join(OUT, "module.wasm")
    subprocess.run(["clang", "--target=wasm32", "-O1", "-g0", "-nostdlib", "-Wl,--no-entry",
                    "-Wl,--export-memory", "-o", wasm, c], check=True)
    readobj = subprocess.run(["llvm-readobj", "--sections", wasm], check=True, capture_output=True, text=True).stdout
sections = re.findall(r"Type: (\w+) \(0x([0-9A-Fa-f]+)\)", readobj)
expected["wasm"] = {"section_ids": [int(h, 16) for _, h in sections],
                    "exports": ["memory", "add", "bump"], "imports": [["env", "host_log"]]}

with open(os.path.join(OUT, "expected.json"), "w") as f:
    json.dump(expected, f, indent=1, sort_keys=True)
print("fixtures written to", OUT)
