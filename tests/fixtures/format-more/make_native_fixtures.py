#!/usr/bin/env python3
"""Builds the native fixtures of tests/fixtures/format-more/native and the
reference values the C++ tests compare against:

  macho-x86_64.o, macho-arm64.o   clang -c for Apple targets; expected
                                  sections and symbols from llvm-readobj
  macho-fat.o                     the two objects in a fat (universal)
                                  container; expected slices from
                                  llvm-objdump --macho --universal-headers
  coff-amd64.obj, coff-i386.obj   clang -c for MSVC targets; expected from
                                  llvm-readobj
  coff.lib                        llvm-lib archive of the amd64 object
  go-linux, go-windows.exe        go build -ldflags=-s -w (stripped);
                                  expected version from `go version -m`,
                                  main.* names from `go tool nm` on an
                                  unstripped build of the same source

Needs clang, llvm-readobj, llvm-objdump, llvm-lib and Go.
"""
import json
import os
import re
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "native")
os.makedirs(OUT, exist_ok=True)
expected = {}


def run(*args, **kw):
    return subprocess.run(list(args), check=True, capture_output=True, text=True, **kw).stdout


C_SOURCE = r'''
extern int cortex_external_helper(int);
int cortex_counter = 7;
__attribute__((section(".text$cortexlongsection"))) int cortex_long_named_function(int value) { return value * 3; }
int cortex_entry(int x) { cortex_counter += x; return cortex_external_helper(cortex_long_named_function(cortex_counter)); }
'''


def readobj(path):
    text = run("llvm-readobj", "--sections", "--symbols", path)
    sections = re.findall(r"^\s+Name: (\S+) \(", text.split("Symbols [")[0], re.M)
    symbols = re.findall(r"^\s+Name: (\S*) ?(?:\(|$)", text.split("Symbols [")[1], re.M) if "Symbols [" in text else []
    return sections, symbols


with tempfile.TemporaryDirectory() as tmp:
    c = os.path.join(tmp, "sample.c")
    with open(c, "w") as f:
        f.write(C_SOURCE)

    # Mach-O: Apple targets do not accept the Windows section name.
    mc = os.path.join(tmp, "macho.c")
    with open(mc, "w") as f:
        f.write(C_SOURCE.replace('__attribute__((section(".text$cortexlongsection"))) ', ''))
    macho = {}
    slices = []
    for arch, target in (("x86_64", "x86_64-apple-macos11"), ("arm64", "arm64-apple-macos11")):
        path = os.path.join(OUT, f"macho-{arch}.o")
        subprocess.run(["clang", "-target", target, "-O1", "-c", mc, "-o", path], check=True)
        sections, symbols = readobj(path)
        macho[arch] = {"sections": sections, "symbols": symbols}
        with open(path, "rb") as f:
            slices.append((arch, f.read()))
    # Fat container: the official header, 4096-byte aligned slices.
    cpu = {"x86_64": (0x01000007, 3), "arm64": (0x0100000C, 0)}
    offset = 4096
    header = struct.pack(">II", 0xCAFEBABE, len(slices))
    body = b""
    for arch, data in slices:
        header += struct.pack(">IIIII", cpu[arch][0], cpu[arch][1], offset, len(data), 12)
        pad = offset - (len(header) + 0) if not body else 0
        body += data + b"\0" * ((-len(data)) % 4096)
        offset += len(data) + ((-len(data)) % 4096)
    fat = header + b"\0" * (4096 - len(header)) + body
    fat_path = os.path.join(OUT, "macho-fat.o")
    with open(fat_path, "wb") as f:
        f.write(fat)
    universal = run("llvm-objdump", "--macho", "--universal-headers", fat_path)
    macho["fat_offsets"] = [int(x) for x in re.findall(r"^\s*offset (\d+)", universal, re.M)]
    macho["fat_cputypes"] = re.findall(r"^\s*cputype (\S+)", universal, re.M)
    expected["macho"] = macho

    coff = {}
    for arch, target in (("amd64", "x86_64-pc-windows-msvc"), ("i386", "i686-pc-windows-msvc")):
        path = os.path.join(OUT, f"coff-{arch}.obj")
        subprocess.run(["clang", "-target", target, "-O1", "-c", c, "-o", path], check=True)
        sections, symbols = readobj(path)
        coff[arch] = {"sections": sections, "symbols": symbols}
    # Relative member name, as in a real .lib (an absolute path is refused as unsafe).
    subprocess.run(["llvm-lib", "/out:coff.lib", "coff-amd64.obj"], check=True, cwd=OUT)
    expected["coff"] = coff

    # Go: stdlib only, so the build needs no network.
    gomod = os.path.join(tmp, "gofix")
    os.makedirs(gomod)
    with open(os.path.join(gomod, "go.mod"), "w") as f:
        f.write("module example.com/cortexfixture\n\ngo 1.21\n")
    with open(os.path.join(gomod, "main.go"), "w") as f:
        f.write('package main\n\nimport "fmt"\n\nfunc cortexScale(v int) int { return v*3 + 1 }\n\n'
                'func main() { fmt.Println("cortex go fixture", cortexScale(14)) }\n')
    env = dict(os.environ, CGO_ENABLED="0", GOFLAGS="-trimpath")
    go = {}
    for name, goos, ext in (("go-linux", "linux", ""), ("go-windows", "windows", ".exe")):
        target = os.path.join(OUT, name + ext)
        e = dict(env, GOOS=goos, GOARCH="amd64")
        subprocess.run(["go", "build", "-ldflags=-s -w", "-o", target, "."], check=True, cwd=gomod, env=e)
        full = os.path.join(tmp, name + "-full" + ext)
        subprocess.run(["go", "build", "-o", full, "."], check=True, cwd=gomod, env=e)
        info = run("go", "version", "-m", target)
        lines = info.splitlines()
        nm = run("go", "tool", "nm", full)
        mains = sorted({m for m in re.findall(r"\sT (main\.\S+)", nm)})
        go[name] = {"version": lines[0].split(": ", 1)[1].strip(),
                    "path": re.search(r"^\tpath\t(\S+)", info, re.M).group(1),
                    "main_module": re.search(r"^\tmod\t(\S+)", info, re.M).group(1),
                    "build": dict(re.findall(r"^\tbuild\t([^=]+)=(.*)$", info, re.M)),
                    "main_functions": mains}
    expected["go"] = go

with open(os.path.join(OUT, "expected.json"), "w") as f:
    json.dump(expected, f, indent=1, sort_keys=True)
print("fixtures written to", OUT)
