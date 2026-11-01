#!/usr/bin/env python3
"""E2E: investigations through the shipped cortex.exe (step 6).

  1. tools/list has the investigation tools and no approval tool;
  2. a function_static investigation runs end to end: Cortex runs the static
     steps, the agent records verified evidence, the investigation completes
     citing it;
  3. a memory_value investigation waits for approval; the agent's result is
     refused until the person approves with `cortex.exe investigation
     approve`; the journal shows who approved;
  4. the CLI lists the investigations and prints the report.

    python tests/investigation_e2e.py --cortex stage/Cortex/cortex.exe \\
        --target stage/Cortex/e2e/cortex_test_target_x64.exe [--runner wine]
"""

import argparse
import json
import os
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

    work = tempfile.mkdtemp(prefix="cortex-investigation-e2e-")
    try:
        root = os.path.join(work, "root")
        os.makedirs(root)
        target = os.path.join(root, "game.exe")
        shutil.copyfile(arguments.target, target)
        path = windows_path(runner, target)
        stores = ["--artifact-root", windows_path(runner, root),
                  "--evidence-store", windows_path(runner, os.path.join(work, "evidence")),
                  "--investigation-store", windows_path(runner, os.path.join(work, "investigations"))]
        server = Mcp(runner + [cortex, "mcp", "--tools", "all", "--allow-state"] + stores, bundle)
        server.initialize()
        tools = {t["name"] for t in (server.request("tools/list", {}) or {}).get("result", {}).get("tools", [])}
        check({"cortex_investigation_create", "cortex_investigation_run_step", "cortex_investigation_step_result",
               "cortex_investigation_conclude"} <= tools and not any("approve" in name for name in tools),
              "investigation tools listed, no approval tool")

        def call(name, args):
            failed, result = server.call(name, dict(args, mutation_permission=True) if name not in (
                "cortex_investigation_get", "cortex_investigation_list", "cortex_investigation_report",
                "cortex_investigation_templates") else args)
            return failed, result

        def revision(identifier):
            return call("cortex_investigation_get", {"id": identifier})[1]["investigation"]["revision"]

        # Static investigation end to end.
        failed, created = call("cortex_investigation_create", {
            "title": "CortexDemoApplyDamage", "question": "Is CortexDemoApplyDamage exported, and who calls it?",
            "workflow": "function_static", "parameters": {"path": path, "function": "CortexDemoApplyDamage"}})
        first = created.get("investigation", {}).get("id", "")
        call("cortex_investigation_start", {"id": first, "expected_revision": 1})
        ran = [call("cortex_investigation_run_step", {"id": first, "index": i, "expected_revision": revision(first)})
               for i in range(3)]
        check(all(not f and r["investigation"]["plan"][i]["status"] == "done" for i, (f, r) in enumerate(ran)),
              "Cortex ran the three static steps")
        failed, catalog = server.call("cortex_artifact_functions", {"path": path, "filter": "CortexDemoApplyDamage"})
        rva = catalog["functions"][0]["rva"]
        failed, evidence = server.call("cortex_evidence_record", {
            "statement": "CortexDemoApplyDamage is exported at this RVA", "class": "verified_observation", "origin": "static",
            "subject": {"path": path, "location": "rva:" + rva},
            "produced_by": {"tool": "cortex_artifact_functions", "arguments": {"path": path, "filter": "CortexDemoApplyDamage"},
                            "expect": [{"pointer": "/functions/0/rva", "value": rva}]},
            "mutation_permission": True})
        evidence_id = evidence.get("record", {}).get("id", "")
        failed, result = call("cortex_investigation_step_result", {
            "id": first, "index": 3, "expected_revision": revision(first), "status": "done",
            "summary": "recorded the export", "references": [{"kind": "evidence", "id": evidence_id}]})
        check(not failed, "agent step recorded with checked evidence", result)
        failed, result = call("cortex_investigation_conclude", {
            "id": first, "expected_revision": revision(first), "state": "completed",
            "conclusion": "Exported; CFG and callers recorded."})
        check(not failed and result["investigation"]["state"] == "completed" and result["investigation"]["evidence"] == [evidence_id],
              "investigation completed citing its evidence")

        # Live investigation: approval comes from the person's CLI.
        failed, created = call("cortex_investigation_create", {
            "title": "Health", "question": "Which instruction writes health?", "workflow": "memory_value",
            "parameters": {"value": "100"}})
        second = created.get("investigation", {}).get("id", "")
        call("cortex_investigation_start", {"id": second, "expected_revision": 1})
        call("cortex_investigation_step_result", {"id": second, "index": 0, "expected_revision": revision(second),
                                                  "status": "done", "summary": "3 candidates",
                                                  "references": [{"kind": "tool_call", "id": "scan_new"}]})
        failed, state = call("cortex_investigation_get", {"id": second})
        check(state["investigation"]["state"] == "awaiting_approval", "live input step waits for approval")
        failed, result = call("cortex_investigation_step_result", {"id": second, "index": 1, "expected_revision": revision(second),
                                                                   "status": "done", "summary": "pressed D"})
        check(failed and result.get("error") == "approval_required", "agent result refused before approval")
        store = windows_path(runner, os.path.join(work, "investigations"))
        approve = subprocess.run(runner + [cortex, "investigation", "approve", second, "1", "--note", "D key once",
                                           "--store", store], cwd=bundle, capture_output=True, text=True, timeout=120)
        check(approve.returncode == 0, "person approves with cortex.exe investigation approve", approve.stdout[-400:])
        failed, result = call("cortex_investigation_step_result", {"id": second, "index": 1, "expected_revision": revision(second),
                                                                   "status": "done", "summary": "health 100 -> 94",
                                                                   "references": [{"kind": "tool_call", "id": "cortex_test_run"}]})
        approvals = [e for e in result.get("investigation", {}).get("journal", []) if e["event"] == "approved"]
        check(not failed and approvals and approvals[0]["actor"] == "cli", "result accepted after the person's approval",
              result)
        server.close()

        listing = subprocess.run(runner + [cortex, "investigation", "list", "--store", store], cwd=bundle,
                                 capture_output=True, text=True, timeout=120)
        rows = json.loads(listing.stdout).get("investigations", []) if listing.stdout.strip().startswith("{") else []
        check(len(rows) == 2, "cortex.exe investigation list")
        report = subprocess.run(runner + [cortex, "investigation", "report", first, "--store", store], cwd=bundle,
                                capture_output=True, text=True, timeout=120)
        check(report.returncode == 0 and "**completed**" in report.stdout and evidence_id in report.stdout,
              "cortex.exe investigation report")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("%d investigation E2E check(s) failed" % len(failures))
        return 1
    print("investigation E2E passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
