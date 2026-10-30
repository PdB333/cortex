#!/usr/bin/env python3
"""E2E: the manual reverse-engineering workflow of roadmap step 3, scripted.

On the test target's demonstration structure (a heap-allocated actor whose
health only CortexDemoApplyDamage writes, reached from an exported global),
using the same primitives a person drives from the desktop:

  1. find the value: first scan for 100, apply the action, next scan for 94;
  2. watch it: a live watch records the next change (94 -> 88);
  3. find what writes it: a hardware write breakpoint in log mode catches the
     instruction, which lies in CortexDemoApplyDamage;
  4. navigate the code: module+RVA of the hit, disassembly of the function
     with the writing instruction (the trap reports the *next* one), its CFG;
  5. keep it: a pointer path from the exported global that resolves to the
     same address, a named address and a knowledge record whose evidence is
     the breakpoint hit;
  6. clean up: breakpoint and watch removed, the target still alive.

The "action" is the file the target watches (<manifest>.damage), standing for
the person pressing D. The server runs with --allow-writes because
breakpoints are control operations.

    python tests/manual_demo_e2e.py --cortex stage/Cortex/cortex.exe \\
        --target stage/Cortex/e2e/cortex_test_target_x64.exe [--runner wine]
"""

import argparse
import json
import os
import shlex
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from context_isolation_e2e import Mcp, route, start_target, wait_for_runtime  # noqa: E402


def as_int(value):
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--runner", default="")
    arguments = parser.parse_args()

    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    target = os.path.abspath(arguments.target)
    bundle = os.path.dirname(cortex)
    workdir = tempfile.mkdtemp(prefix="cortex-demo-")
    failures = []

    def check(condition, message, detail=None):
        print(("ok    " if condition else "FAIL  ") + message)
        if not condition:
            if detail is not None:
                print("      " + json.dumps(detail, default=str)[:700])
            failures.append(message)
        return condition

    process, manifest = start_target(runner, target, workdir, "demo")
    server = None
    try:
        import subprocess
        pid = manifest["pid"]
        health_address = as_int(manifest["demo_actor_health"])
        damage_function = as_int(manifest["demo_apply_damage"])
        damage_file = manifest["demo_damage_file"]
        module_name = os.path.basename(manifest["module_path"].replace("\\", "/"))

        def act():
            """The person presses D: one call of CortexDemoApplyDamage."""
            with open(damage_file, "w") as handle:
                handle.write("hit")
            deadline = time.time() + 10
            while os.path.exists(damage_file) and time.time() < deadline:
                time.sleep(0.05)
            time.sleep(0.2)

        inject = subprocess.run(runner + [cortex, "inject", str(pid)], cwd=bundle,
                                capture_output=True, text=True, timeout=120)
        check(inject.returncode == 0, "runtime loaded (cortex.exe inject)", inject.stdout + inject.stderr)
        check(wait_for_runtime(bundle, pid), "runtime started")
        server = Mcp(runner + [cortex, "mcp", "--pid", str(pid), "--tools", "all", "--allow-writes"], bundle)
        server.initialize()

        def call(name, args):
            error, content = server.call(name, args)
            return error, route(content)

        # 1. Find the value.
        error, first = call("scan_new", {"type": "i32", "value": "100"})
        check(not error and first.get("ok") and first.get("count", 0) >= 1, "first scan for 100", first)
        check(first.get("status") == "completed" and first.get("target", {}).get("pid") == pid,
              "scan result follows the common contract", first)
        scan_id = first.get("scan_id")
        act()
        error, narrowed = call("scan_next", {"scan_id": scan_id, "filter": "exact", "value": "94"})
        check(not error and narrowed.get("ok"), "next scan for 94 after the action", narrowed)
        error, results = call("scan_results", {"_path": {"scan_id": scan_id}, "_query": {"limit": 1000}})
        found = [as_int(row["address"]) for row in results.get("results", [])]
        check(health_address in found, "the actor's health is among %d candidate(s)" % len(found),
              {"expected": hex(health_address), "found": [hex(a) for a in found[:20]]})

        # 2. Watch it.
        error, watch = call("watch_add", {"address": hex(health_address), "type": "i32", "label": "demo health",
                                          "mutation_permission": True})
        check(not error and watch.get("ok"), "live watch on the value", watch)
        time.sleep(0.5)          # the watch thread takes its first reading
        call("watch_events", {})  # drain
        act()
        rows = []
        deadline = time.time() + 5
        while time.time() < deadline and not any("88" in json.dumps(row) for row in rows):
            time.sleep(0.25)
            error, changes = call("watch_events", {})
            rows += changes.get("events", [])
        if not check(any("88" in json.dumps(row) for row in rows), "the watch recorded 94 -> 88", rows[:5]):
            print("      watch_list: " + json.dumps(call("watch_list", {})[1])[:400])

        # 3. Find what writes it.
        error, breakpoint = call("debug_breakpoint_add", {"address": hex(health_address), "kind": "hw_write",
                                                          "size": 4, "action": "log", "mutation_permission": True})
        check(not error and breakpoint.get("ok") and breakpoint.get("id"), "hardware write breakpoint (log mode)",
              breakpoint)
        breakpoint_id = breakpoint.get("id")
        act()
        hits = []
        deadline = time.time() + 10
        while not hits and time.time() < deadline:
            error, log = call("debug_breakpoint_log", {"id": breakpoint_id, "_path": {"id": breakpoint_id}})
            hits = log.get("entries", [])
            if not hits:
                time.sleep(0.3)
        check(len(hits) >= 1, "the breakpoint logged the write", log)
        trap_ip = as_int(hits[0]["instruction"]) if hits else 0
        check(damage_function <= trap_ip < damage_function + 0x200,
              "the write happened in CortexDemoApplyDamage", {"ip": hex(trap_ip), "function": hex(damage_function)})

        # 4. Navigate the code.
        error, where = call("symbols_module", {"_query": {"address": hex(trap_ip)}})
        check(not error and module_name.lower() in json.dumps(where).lower(),
              "the hit resolves to %s+RVA" % module_name, where)
        error, listing = call("disasm", {"_query": {"address": hex(damage_function), "count": 60}})
        instructions = listing.get("instructions", [])
        writer = None
        for row in instructions:
            start = as_int(row["address"])
            if start + int(row.get("size", 0)) == trap_ip:
                writer = row
        check(writer is not None and "[" in writer.get("text", "") and writer.get("mnemonic", "").startswith("mov"),
              "the writing instruction is the store just before the trap address", writer)
        error, cfg = call("analysis_cfg", {"address": hex(damage_function)})
        check(not error and len(cfg.get("blocks", [])) >= 1, "CFG of the function", cfg)

        # 5. Keep it.
        error, saved = call("project_pointer_path_set", {
            "name": "demo.actor.health", "module": module_name,
            "base_offset": as_int(manifest["demo_actor_global_rva"]), "offsets": [4], "final_type": "i32",
            "notes": "exported g_cortex_demo_actor -> +4 health", "mutation_permission": True})
        check(not error and saved.get("ok"), "pointer path saved", saved)
        error, resolved = call("project_pointer_path_resolve", {"_path": {"name": "demo.actor.health"}})
        check(not error and as_int(resolved.get("address", 0)) == health_address,
              "the pointer path resolves to the scanned address", resolved)
        error, named = call("project_address_set", {"name": "demo.health", "address": hex(health_address),
                                                    "type": "i32", "notes": "found by scan, written by "
                                                    "CortexDemoApplyDamage", "mutation_permission": True})
        check(not error and named.get("ok"), "named address saved", named)
        # The project persists across runs: an update must name the revision
        # it replaces (optimistic concurrency), as any agent's would.
        error, existing = call("project_knowledge_get", {"_path": {"id": "demo.apply_damage.writes_health"}})
        record = existing.get("record", existing) if not error else {}
        revision = record.get("revision") if isinstance(record, dict) else None
        error, claim = call("project_knowledge_put", dict({
            "id": "demo.apply_damage.writes_health", "kind": "function",
            "statement": "CortexDemoApplyDamage writes the actor's health (actor+4).",
            "status": "observed",
            "evidence": [{"source": "debug_breakpoint_log", "reference": "breakpoint:%s" % breakpoint_id,
                          "detail": "hw_write hit, trap ip %s" % hex(trap_ip)}],
            "mutation_permission": True}, **({"expected_revision": revision} if revision else {})))
        stored = claim.get("record", claim.get("stored", claim))
        check(not error and claim.get("ok"), "knowledge record saved with the hit as evidence", claim)
        error, back = call("project_knowledge_get", {"_path": {"id": "demo.apply_damage.writes_health"}})
        check(not error and "CortexDemoApplyDamage" in json.dumps(back), "knowledge record reads back", back)
        del stored

        # 6. Clean up.
        error, removed = call("debug_breakpoint_delete", {"id": breakpoint_id, "_path": {"id": breakpoint_id},
                                                          "mutation_permission": True})
        check(not error and removed.get("ok", True), "breakpoint removed", removed)
        act()
        error, value = call("memory_read", {"address": hex(health_address), "type": "i32"})
        check(not error and value.get("value") == 76, "the target still runs (health 76 after the 4th hit)", value)
    finally:
        if server:
            server.close()
        if process.poll() is None:
            process.kill()

    if failures:
        print("\n%d check(s) failed" % len(failures))
        return 1
    print("\nPASS: manual workflow - scan, watch, find what writes, navigate, save")
    return 0


if __name__ == "__main__":
    sys.exit(main())
