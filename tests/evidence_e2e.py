#!/usr/bin/env python3
"""E2E: evidence records through the shipped cortex.exe (step 5).

  1. without --allow-state the writers are refused by the host;
  2. a static claim Cortex reruns and reproduces is recorded as
     verified_observation; an unsupported class is refused with the reason;
  3. after the server restarts (no target, no process), the same records
     read back unchanged and reading creates no file;
  4. export as Markdown over MCP and with `cortex.exe evidence export`;
  5. comparison with a copy whose hash differs but whose code does not.

    python tests/evidence_e2e.py --cortex stage/Cortex/cortex.exe \\
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

    work = tempfile.mkdtemp(prefix="cortex-evidence-e2e-")
    try:
        root = os.path.join(work, "root")
        store = os.path.join(work, "evidence")
        os.makedirs(root)
        target = os.path.join(root, "game.exe")
        shutil.copyfile(arguments.target, target)
        padded = os.path.join(root, "game_repacked.exe")
        shutil.copyfile(arguments.target, padded)
        with open(padded, "ab") as handle:
            handle.write(b"\0" * 4096)
        path = windows_path(runner, target)

        def server(*extra):
            s = Mcp(runner + [cortex, "mcp", "--tools", "all", "--artifact-root", windows_path(runner, root),
                              "--evidence-store", windows_path(runner, store)] + list(extra), bundle)
            s.initialize()
            return s

        s = server()
        failed, functions = s.call("cortex_artifact_functions", {"path": path, "filter": "CortexDemoApplyDamage"})
        rva = functions["functions"][0]["rva"]
        record = {"statement": "CortexDemoApplyDamage is an exported function at this RVA", "class": "verified_observation",
                  "origin": "static", "subject": {"path": path, "location": "rva:" + rva},
                  "produced_by": {"tool": "cortex_artifact_functions", "arguments": {"path": path, "filter": "CortexDemoApplyDamage"},
                                  "expect": [{"pointer": "/functions/0/name", "value": "CortexDemoApplyDamage"},
                                             {"pointer": "/functions/0/rva", "value": rva}]},
                  "mutation_permission": True}
        failed, result = s.call("cortex_evidence_record", record)
        refusal = result.get("error")
        refusal = refusal.get("code") if isinstance(refusal, dict) else refusal
        check(failed and refusal == "state_authority_required", "writers refused without --allow-state", result)
        s.close()

        s = server("--allow-state")
        failed, result = s.call("cortex_evidence_record", record)
        saved = result.get("record", {})
        check(not failed and saved.get("class") == "verified_observation" and
              saved.get("verification", {}).get("status") == "reproduced" and
              saved.get("subject", {}).get("identity") == "computed_by_cortex" and "fingerprint" in saved.get("subject", {}),
              "static claim reproduced by Cortex and recorded as verified_observation", result)
        unsupported = dict(record, statement="claimed without a basis")
        unsupported.pop("produced_by")
        failed, result = s.call("cortex_evidence_record", unsupported)
        check(failed and result.get("error") == "class_requirements_not_met" and result.get("reason"),
              "a class without its basis is refused with the reason")
        failed, result = s.call("cortex_evidence_record", {"statement": "the damage is applied once per key press",
                                                           "class": "hypothesis", "origin": "observed",
                                                           "subject": {"path": path, "location": "rva:" + rva},
                                                           "relations": [{"type": "correlates", "to": {"evidence": saved.get("id", "")}}],
                                                           "mutation_permission": True})
        check(not failed, "a hypothesis related to the verified record")
        failed, listed = s.call("cortex_evidence_list", {})
        s.close()
        files = sorted(os.listdir(store))

        # A new server process, no target: the same records, nothing new written.
        s = server()
        failed, again = s.call("cortex_evidence_list", {})
        check(not failed and again.get("records") == listed.get("records") and len(again.get("records", [])) == 2,
              "records read back unchanged after a restart, without any process")
        failed, one = s.call("cortex_evidence_get", {"id": saved.get("id", "")})
        check(not failed and one["record"]["revision"] == 1, "get after restart")
        failed, report = s.call("cortex_evidence_export", {"format": "markdown"})
        document = report.get("document", "")
        check(not failed and "**verified_observation**" in document and "not a proof" in document, "Markdown export")
        failed, compared = s.call("cortex_evidence_compare", {"sha256": saved["subject"]["sha256"],
                                                              "path": windows_path(runner, padded)})
        check(not failed and compared.get("same_binary") is False and compared.get("counts", {}).get("unchanged_at_rva") == 2,
              "comparison with a repacked copy: code unchanged at the RVA", compared)
        s.close()
        check(sorted(os.listdir(store)) == files, "reading, listing, exporting and comparing created no file")

        run = subprocess.run(runner + [cortex, "evidence", "export", "--store", windows_path(runner, store), "--format", "json"],
                             cwd=bundle, capture_output=True, text=True, timeout=120)
        exported = json.loads(run.stdout) if run.stdout.strip().startswith("{") else {}
        check(run.returncode == 0 and exported.get("schema") == "cortex.evidence.export.v1" and len(exported.get("records", [])) == 2,
              "cortex.exe evidence export")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("%d evidence E2E check(s) failed" % len(failures))
        return 1
    print("evidence E2E passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
