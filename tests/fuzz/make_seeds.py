"""Build the seed corpora of the fuzzers from the repository's fixtures.

usage: python make_seeds.py OUT_DIR

fuzz_artifact inputs start with one byte that picks the file name (see
fuzz_artifact.cpp); fuzz_text inputs start with one byte that picks the
decoder (see fuzz_text.cpp).
"""
import base64
import glob
import json
import os
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
NAMES = {"bin": 0, "dll": 1, "exe": 2, "asar": 3, "js": 4, "map": 5, "zip": 7, "jar": 8, "ts": 9}


def put(out, corpus, name, data):
    os.makedirs(os.path.join(out, corpus), exist_ok=True)
    with open(os.path.join(out, corpus, name), "wb") as handle:
        handle.write(data)


def main():
    out = sys.argv[1]
    fixtures = os.path.join(ROOT, "tests", "fixtures")
    files = [
        (os.path.join(fixtures, "electron-app.asar"), "asar"),
        (os.path.join(fixtures, "dotnet", "Cortex.Fixture.dll"), "dll"),
        (os.path.join(fixtures, "java", "Cortex.Fixture.jar"), "jar"),
        (os.path.join(fixtures, "elf", "actor.elf"), "bin"),
    ]
    files += [(p, "exe") for p in glob.glob(os.path.join(ROOT, "tests", "fuzz", "seeds", "pe", "*.exe"))]
    files += [(p, "js") for p in sorted(glob.glob(os.path.join(fixtures, "**", "*.js"), recursive=True))[:20]]
    files += [(p, "ts") for p in sorted(glob.glob(os.path.join(fixtures, "**", "*.ts"), recursive=True))[:5]]
    files += [(p, "map") for p in sorted(glob.glob(os.path.join(fixtures, "**", "*.map"), recursive=True))[:5]]
    for index, (path, kind) in enumerate(files):
        if not os.path.exists(path):
            continue
        data = open(path, "rb").read()
        put(out, "artifact", "seed%d" % index, bytes([NAMES[kind]]) + data)
        if kind in ("dll", "exe") and len(data) <= 65536:
            put(out, "pe_code", "seed%d" % index, data)
        if kind in ("js", "ts"):
            put(out, "text", "js%d" % index, bytes([0 if kind == "js" else 4]) + data[:200000])
        if kind == "map":
            put(out, "text", "map%d" % index, bytes([1]) + data)
    put(out, "text", "inflate", bytes([3]) + zlib.compress(b"hello cortex " * 64)[2:-4])
    put(out, "text", "dataurl", bytes([2]) + b"data:application/json;base64," +
        base64.b64encode(b'{"version":3,"sources":["a.ts"],"mappings":"AAAA"}'))
    put(out, "text", "map", bytes([1]) + json.dumps(
        {"version": 3, "sources": ["a.ts"], "names": [], "mappings": "AAAA;AACA,CAAC"}).encode())
    for corpus in sorted(os.listdir(out)):
        print(corpus, len(os.listdir(os.path.join(out, corpus))))


if __name__ == "__main__":
    main()
