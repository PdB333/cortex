#!/usr/bin/env python3
"""E2E: the CDP live provider through the shipped cortex.exe (step 8).

Starts tests/fixtures/node-live/app.js with `node --inspect=127.0.0.1:<port>`
and drives the cortex_live_* MCP tools and `cortex.exe live`:

  - a port the operator did not allow (--inspect-port) is refused;
  - targets, attach, scripts and the exact source of the fixture;
  - without --allow-writes a breakpoint is refused (write_authority_required),
    even with mutation_permission;
  - with --allow-writes a log breakpoint on the health write records hits
    whose locals follow the fixture's rule, then removes itself; the program
    keeps running (console ticks continue) and a `debugger;` statement
    Cortex did not set is recorded and resumed;
  - detach, and `cortex.exe live watch` on the same line.

    python tests/live_e2e.py --cortex stage/Cortex/cortex.exe [--node node] [--runner wine]

Exit code 0 = pass.
"""

import argparse
import json
import os
import random
import shlex
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from artifact_mcp_e2e import Mcp  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "node-live", "app.js")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--node", default="node")
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
                print("      " + json.dumps(detail)[:1500])

    with open(FIXTURE, encoding="utf-8") as handle:
        source_lines = handle.read().split("\n")
    line = next(i + 1 for i, text in enumerate(source_lines) if "CORTEX_HEALTH_WRITE" in text)
    port = random.randint(9400, 9900)
    node = subprocess.Popen([arguments.node, "--inspect=127.0.0.1:%d" % port, FIXTURE],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        read_only = Mcp(runner + [cortex, "mcp", "--tools", "all", "--inspect-port", str(port)], bundle)
        read_only.initialize()
        failed, result = read_only.call("cortex_live_targets", {"port": port + 1})
        check(failed and result.get("error") == "port_not_allowed", "a port the operator did not allow is refused", result)
        failed, result = read_only.call("cortex_live_targets", {"port": port})
        targets = result.get("targets", [])
        check(not failed and targets and targets[0].get("type") == "node", "inspector targets listed", result)
        failed, attached = read_only.call("cortex_live_attach", {"port": port})
        session = attached.get("session_id", "")
        check(not failed and attached.get("connected") is True, "attached", attached)
        time.sleep(0.5)
        failed, scripts = read_only.call("cortex_live_scripts", {"session_id": session, "query": "app.js"})
        url = (scripts.get("scripts") or [{}])[0].get("url", "")
        check(url.startswith("file://") and url.endswith("app.js"), "the fixture script is listed", scripts)
        failed, source = read_only.call("cortex_live_source", {"session_id": session, "script": url, "first_line": line, "lines": 1})
        check("CORTEX_HEALTH_WRITE" in source.get("text", ""), "source line %d is the health write" % line, source)
        failed, refused = read_only.call("cortex_live_breakpoint_set", {"session_id": session, "url": url, "line": line,
                                                                        "mutation_permission": True})
        check(failed and refused.get("error") == "write_authority_required",
              "without --allow-writes a breakpoint is refused", refused)
        failed, detached = read_only.call("cortex_live_detach", {"session_id": session})
        check(detached.get("detached") is True, "detached")
        read_only.close()

        server = Mcp(runner + [cortex, "mcp", "--tools", "all", "--allow-writes", "--inspect-port", str(port)], bundle)
        server.initialize()
        failed, attached = server.call("cortex_live_attach", {"port": port})
        session = attached.get("session_id", "")
        failed, missing = server.call("cortex_live_breakpoint_set", {"session_id": session, "url": url, "line": line,
                                                                    "mutation_permission": False})
        check(failed and missing.get("error") == "mutation_permission_required", "mutation_permission is still required", missing)
        failed, bp = server.call("cortex_live_breakpoint_set", {"session_id": session, "url": url, "line": line, "max_hits": 5,
                                                               "mutation_permission": True})
        check(not failed and bp.get("resolved"), "log breakpoint set and resolved", bp)
        deadline = time.time() + 20
        hits = {}
        while time.time() < deadline:
            failed, hits = server.call("cortex_live_hits", {"session_id": session, "breakpoint": bp.get("breakpoint", 0)})
            if hits.get("hits") == 5:
                break
            time.sleep(0.3)
        check(hits.get("hits") == 5 and hits.get("active") is False, "5 hits, then the breakpoint removed itself", hits)
        previous = None
        rule = True
        for record in hits.get("records", []):
            top = record["frames"][0]
            locals_ = top.get("locals", {})
            health = next((int(p["value"]) for p in locals_.get("target", {}).get("preview", []) if p["name"] == "health"), None)
            absorbed = locals_.get("absorbed", {}).get("value")
            if top.get("function") != "applyDamage" or locals_.get("amount", {}).get("value") != 9 or health is None:
                rule = False
            if previous and health not in (previous[0] - (9 - previous[1]), 100):
                rule = False
            previous = (health, absorbed)
        check(rule, "each hit's locals follow the fixture's rule (health drops by amount - absorbed, or resets)", hits)
        deadline = time.time() + 15
        status = {}
        while time.time() < deadline:
            failed, status = server.call("cortex_live_status", {"session_id": session})
            if status.get("foreign_pauses", 0) >= 1:
                break
            time.sleep(0.5)
        check(status.get("foreign_pauses", 0) >= 1, "a debugger statement Cortex did not set was recorded and resumed", status)
        failed, console = server.call("cortex_live_console", {"session_id": session, "limit": 500})
        first = console.get("total", 0)
        time.sleep(1.0)
        failed, console = server.call("cortex_live_console", {"session_id": session, "limit": 1})
        check(console.get("total", 0) > first + 10, "the program keeps running (console ticks continue)", console)
        failed, detached = server.call("cortex_live_detach", {"session_id": session})
        check(detached.get("detached") is True, "detached after the breakpoint")
        server.close()

        run = subprocess.run(runner + [cortex, "live", "watch", "--port", str(port), "--url", url, "--line", str(line),
                                       "--hits", "3", "--seconds", "20"], cwd=bundle, capture_output=True, text=True,
                             encoding="utf-8", timeout=120)
        report = json.loads(run.stdout) if run.stdout.strip().startswith("{") else {}
        check(run.returncode == 0 and report.get("hits") == 3, "cortex.exe live watch records 3 hits", run.stdout[-1500:])
        check(node.poll() is None, "the fixture is still running after every session")
    finally:
        node.kill()
    print("live E2E: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
