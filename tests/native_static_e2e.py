#!/usr/bin/env python3
"""E2E: offline native code analysis through the shipped cortex.exe, checked
against objdump (an independent disassembler).

  1. every direct call target objdump finds in .text is in the function
     catalog (`cortex.exe inspect --functions`);
  2. for a fixed sample of objdump's direct calls and address operands
     (RIP-relative lea/mov on x64, absolute or immediate addresses on x86),
     cortex_artifact_xrefs lists the same instruction as a reference;
  3. the CFG of CortexDemoApplyDamage has the health write and coverage;
  4. annotations: refused without --allow-state, then recorded with it,
     shown in the catalog, and refused on a stale revision.

    python tests/native_static_e2e.py --cortex stage/Cortex/cortex.exe \\
        --target stage/Cortex/e2e/cortex_test_target_x64.exe --objdump objdump [--runner wine]
"""

import argparse
import json
import os
import random
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from artifact_mcp_e2e import Mcp, windows_path  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--objdump", required=True, help="objdump that reads PE files (MinGW binutils)")
    parser.add_argument("--runner", default="")
    arguments = parser.parse_args()
    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    bundle = os.path.dirname(cortex)
    failures = []

    def check(condition, message, detail=None):
        print(("PASS " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)
            if detail is not None:
                print("      " + json.dumps(detail)[:800])

    root = tempfile.mkdtemp(prefix="cortex-native-e2e-")
    try:
        target = os.path.join(root, os.path.basename(arguments.target))
        shutil.copyfile(arguments.target, target)
        headers = subprocess.run([arguments.objdump, "-p", target], capture_output=True, text=True).stdout
        base = int(re.search(r"ImageBase\s+([0-9a-fA-F]+)", headers).group(1), 16)
        size = int(re.search(r"SizeOfImage\s+([0-9a-fA-F]+)", headers).group(1), 16)
        x64 = "pe-x86-64" in headers or "x86-64" in headers

        listing = subprocess.run([arguments.objdump, "-d", "-j", ".text", target], capture_output=True, text=True).stdout
        calls, addresses = set(), set()
        for line in listing.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(\S+)\s+(.*)$", line)
            if not m:
                continue
            at, mnemonic, operands = int(m.group(1), 16), m.group(2), m.group(3)
            if mnemonic.startswith("call"):
                d = re.match(r"([0-9a-f]+)\s*<", operands)
                if d and base <= int(d.group(1), 16) < base + size:
                    calls.add((at - base, int(d.group(1), 16) - base))
            elif x64:
                d = re.search(r"\(%rip\).*#\s*([0-9a-f]+)", operands)
                if d and base <= int(d.group(1), 16) < base + size:
                    addresses.add((at - base, int(d.group(1), 16) - base))
            else:
                d = re.search(r"\$0x([0-9a-f]+)", operands) or re.search(r"(?<![(%\w])0x([0-9a-f]+)(?![(\w])", operands)
                if d and base + 0x1000 <= int(d.group(1), 16) < base + size and mnemonic in ("push", "mov", "movl", "lea", "cmp", "cmpl", "pushl"):
                    addresses.add((at - base, int(d.group(1), 16) - base))

        run = subprocess.run(runner + [cortex, "inspect", windows_path(runner, target), "--functions", "--max", "65536"],
                             cwd=bundle, capture_output=True, text=True, timeout=300)
        catalog = json.loads(run.stdout) if run.stdout.strip().startswith("{") else {}
        starts = {int(f["rva"], 16) for f in catalog.get("functions", [])}
        missing = sorted({t for _, t in calls} - starts)
        check(catalog.get("ok") and len(calls) > 1000 and not missing,
              "all %d objdump call targets are in the catalog (%d functions)" % (len({t for _, t in calls}), len(starts)),
              [hex(x) for x in missing[:10]])
        check(catalog.get("architecture") == ("x64" if x64 else "x86"), "architecture")

        store = os.path.join(root, "annotations")
        server = Mcp(runner + [cortex, "mcp", "--tools", "all", "--artifact-root", windows_path(runner, root),
                               "--annotation-store", windows_path(runner, store)], bundle)
        server.initialize()
        sample = random.Random(1234)
        picked = sample.sample(sorted(calls), min(60, len(calls))) + sample.sample(sorted(addresses), min(60, len(addresses)))
        missed = []
        for source, destination in picked:
            failed, result = server.call("cortex_artifact_xrefs", {"path": windows_path(runner, target),
                                                                   "target": hex(destination), "max_entries": 65536})
            froms = {int(ref["from"], 16) for ref in result.get("references", [])}
            if failed or source not in froms:
                missed.append([hex(source), hex(destination)])
        check(len(addresses) > 50 and not missed,
              "xrefs match objdump on %d sampled calls and address operands" % len(picked), missed[:10])

        failed, cfg = server.call("cortex_artifact_cfg", {"path": windows_path(runner, target), "function": "CortexDemoApplyDamage",
                                                          "covered_rvas": [catalog and next(
                                                              (f["rva"] for f in catalog["functions"] if f["name"] == "CortexDemoApplyDamage"), "0x0")]})
        texts = [i["text"] for b in cfg.get("blocks", []) for i in b["instructions"]]
        check(not failed and any("+0x04]" in t and t.split(",")[0].find("[") >= 0 for t in texts) and
              cfg.get("coverage", {}).get("blocks_covered") == 1, "CFG of CortexDemoApplyDamage: health write, coverage")

        location = next(f["rva"] for f in catalog["functions"] if f["name"] == "CortexDemoApplyDamage")
        failed, result = server.call("cortex_artifact_annotate", {"path": windows_path(runner, target), "location": "rva:" + location,
                                                                  "kind": "name", "text": "apply_damage", "mutation_permission": True})
        refusal = result.get("error")
        refusal = refusal.get("code") if isinstance(refusal, dict) else refusal
        check(failed and refusal == "state_authority_required", "annotate refused without --allow-state", result)
        server.close()

        server = Mcp(runner + [cortex, "mcp", "--tools", "all", "--allow-state", "--artifact-root", windows_path(runner, root),
                               "--annotation-store", windows_path(runner, store)], bundle)
        server.initialize()
        failed, result = server.call("cortex_artifact_annotate", {"path": windows_path(runner, target), "location": "rva:" + location,
                                                                  "kind": "name", "text": "apply_damage", "mutation_permission": False})
        check(failed and result.get("error") == "mutation_permission_required", "annotate needs mutation_permission")
        failed, result = server.call("cortex_artifact_annotate", {"path": windows_path(runner, target), "location": "rva:" + location,
                                                                  "kind": "name", "text": "apply_damage", "expected_revision": 0,
                                                                  "mutation_permission": True})
        check(not failed and result.get("revision") == 1, "annotation recorded with --allow-state", result)
        failed, result = server.call("cortex_artifact_functions", {"path": windows_path(runner, target), "filter": "apply_damage"})
        check(not failed and [f["name"] for f in result.get("functions", [])] == ["apply_damage"], "catalog shows the annotation")
        failed, result = server.call("cortex_artifact_annotate", {"path": windows_path(runner, target), "location": "rva:" + location,
                                                                  "kind": "comment", "text": "late", "expected_revision": 0,
                                                                  "mutation_permission": True})
        check(failed and result.get("error") == "revision_conflict", "stale revision refused")
        failed, result = server.call("cortex_artifact_annotations", {"path": windows_path(runner, target)})
        check(not failed and result.get("revision") == 1 and result["annotations"][0]["source"] == "agent",
              "annotations read back with their source")
        server.close()
        check(len(os.listdir(store)) == 1, "one annotation file, named by the artifact's SHA-256")
    finally:
        shutil.rmtree(root, ignore_errors=True)

    if failures:
        print("%d native static E2E check(s) failed" % len(failures))
        return 1
    print("native static E2E passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
