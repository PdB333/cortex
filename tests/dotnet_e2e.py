#!/usr/bin/env python3
"""E2E: .NET metadata and CIL through the shipped cortex.exe.

Compares `cortex.exe inspect` and the MCP tools cortex_artifact_inspect /
cortex_artifact_cil with the answers of System.Reflection.Metadata
(tests/fixtures/dotnet/Oracle). CI passes a freshly built assembly, package
and oracle output (make_dotnet_fixture.py --out); without arguments the
committed fixtures are used.

    python tests/dotnet_e2e.py --cortex stage/Cortex/cortex.exe \\
        [--fixtures DIR] [--runner wine]

Exit code 0 = pass.
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
    parser.add_argument("--fixtures", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "dotnet"))
    parser.add_argument("--runner", default="")
    arguments = parser.parse_args()
    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    bundle = os.path.dirname(cortex)
    with open(os.path.join(arguments.fixtures, "Cortex.Fixture-expected.json"), encoding="utf-8") as handle:
        expected = json.load(handle)
    failures = []

    def check(condition, message, detail=None):
        print(("PASS " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)
            if detail is not None:
                print("      " + json.dumps(detail)[:600])

    root = tempfile.mkdtemp(prefix="cortex-dotnet-e2e-")
    try:
        dll = os.path.join(root, "Cortex.Fixture.dll")
        shutil.copyfile(os.path.join(arguments.fixtures, "Cortex.Fixture.dll"), dll)
        package = os.path.join(root, "Cortex.Fixture.1.4.2.nupkg")
        shutil.copyfile(os.path.join(arguments.fixtures, "Cortex.Fixture.1.4.2.nupkg"), package)

        # CLI
        run = subprocess.run(runner + [cortex, "inspect", windows_path(runner, dll)], cwd=bundle,
                             capture_output=True, text=True, encoding="utf-8", timeout=120)
        report = json.loads(run.stdout) if run.stdout.strip().startswith("{") else {}
        net = report.get("data", {}).get("dotnet", {})
        check(report.get("provider", {}).get("name") == "dotnet", "inspect picks the dotnet provider")
        check(net.get("assembly", {}).get("version") == expected["assembly"]["version"] and
              net.get("module") == expected["module"], "assembly identity matches System.Reflection.Metadata")
        check(net.get("types") == expected["types"], "types, fields, methods and signatures match", net.get("types"))
        check(net.get("member_refs") == expected["member_refs"], "member references match")
        check(net.get("pinvoke") == expected["pinvoke"], "P/Invoke matches")
        run = subprocess.run(runner + [cortex, "inspect", windows_path(runner, dll), "--cil", "Cortex.Fixture.Actor::Heal"],
                             cwd=bundle, capture_output=True, text=True, encoding="utf-8", timeout=120)
        cil = json.loads(run.stdout) if run.stdout.strip().startswith("{") else {}
        heal = next(body for body in expected["method_bodies"] if body["method"] == "Cortex.Fixture.Actor::Heal")
        got = (cil.get("methods") or [{}])[0]
        check(got.get("instructions") == heal["instructions"] and got.get("exception_clauses") == heal["exception_clauses"],
              "inspect --cil matches the oracle for Actor::Heal", got)

        # MCP, every method body, and the assembly inside the package.
        server = Mcp(runner + [cortex, "mcp", "--tools", "all", "--artifact-root", windows_path(runner, root)], bundle)
        server.initialize()
        mismatched = []
        for body in expected["method_bodies"]:
            failed, result = server.call("cortex_artifact_cil", {"path": windows_path(runner, dll), "method": body["token"]})
            method = (result.get("methods") or [{}])[0]
            if failed or method.get("instructions") != body["instructions"] or method.get("max_stack") != body["max_stack"]:
                mismatched.append(body["method"])
        check(not mismatched, "cortex_artifact_cil matches the oracle for all %d bodies" % len(expected["method_bodies"]),
              mismatched)
        failed, result = server.call("cortex_artifact_inspect", {"path": windows_path(runner, package)})
        data = result.get("data", {})
        check(not failed and result.get("format") == "nupkg" and data.get("package", {}).get("id") == "Cortex.Fixture" and
              not data.get("crc", {}).get("mismatches"), "NuGet package recognised, CRCs match")
        failed, result = server.call("cortex_artifact_cil", {"path": windows_path(runner, package),
                                                             "child": "lib/net8.0/Cortex.Fixture.dll",
                                                             "method": "Cortex.Fixture.Actor::Classify"})
        classify = next(body for body in expected["method_bodies"] if body["method"] == "Cortex.Fixture.Actor::Classify")
        check(not failed and (result.get("methods") or [{}])[0].get("instructions") == classify["instructions"],
              "CIL of the assembly inside the package (deflated entry)")
        failed, result = server.call("cortex_artifact_cil", {"path": windows_path(runner, dll), "method": "Ticks"})
        check((result.get("methods") or [{}])[0].get("reason") == "method_has_no_body", "P/Invoke has no body")
        server.close()
    finally:
        shutil.rmtree(root, ignore_errors=True)

    if failures:
        print("%d .NET E2E check(s) failed" % len(failures))
        return 1
    print(".NET E2E passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
