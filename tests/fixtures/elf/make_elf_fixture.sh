#!/bin/sh
# Rebuilds actor.elf and actor-expected.json (function symbols from nm).
set -eu
cd "$(dirname "$0")"
gcc -O1 -fPIE -pie -fno-omit-frame-pointer -o actor.elf actor.c
python3 - <<'PY'
import json, subprocess
out = subprocess.run(["nm", "--defined-only", "actor.elf"], capture_output=True, text=True, check=True).stdout
functions = {}
for line in out.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in ("T", "t"):
        functions[parts[2]] = "0x%x" % int(parts[0], 16)
json.dump({"format": "elf", "machine": "x86-64", "functions": functions}, open("actor-expected.json", "w"), indent=2, sort_keys=True)
PY
